"""The Dreamcast menu's pictures, fonts and title music, from the Xbox game's
ui.map (port/dc/src/dc_menu.c, dc_hud.c, dc_sound.c).

The menu is the N64's (port/n64/src/n64_menu.c, ported); from Halo it takes
what tools/n64/ui.py takes for the N64, but as the Dreamcast's own files:

- hud/ui.dch, in the HUD's format (tools/n64/hud_dc.py: dc_hud.c loads it
  as a map's HUD, n64_hud_set_map("ui")), the sprites in a fixed order
  (SPRITE_* below, the same numbers in dc_menu.c):

      0        Halo's logo (its letters and the drawing's lines), cropped,
               LOGO_WIDTH across, ARGB4444 premultiplied
      1        Blood Gulch's picture (mp_map_grafix's tenth: the one map
               the Dreamcast plays), 256x128 RGB565
      2-6      the game types' (game_type_grafix: CTF, King, Slayer,
               Oddball, Race), 256x128 RGB565

  and two fonts, large_ui (the "HUD's font": dc_hud.c's own text) and
  small_ui, in one 4-bit atlas. Behind the menu is Blood Gulch itself
  (dc_menu.c flies over its level), so no backdrop picture;

- music/title.dca, Halo's title music (sound\\music\\title1\\title1, as
  tools/n64/music.py finds it: the start "in", the monks, once; then its
  three "loops", one at a time, a random one each time): every version
  decoded from Xbox ADPCM, kept stereo at 44100 Hz, each channel made the
  AICA's 4-bit ADPCM (sounds_dc.py aica_adpcm, from the decoder's resting
  state at each version's start). dc_sound.c streams it from the disc and
  decodes it on the SH-4 into 16-bit rings the AICA loops over (a channel
  plays at most 65535 samples; the AICA's own ADPCM would carry the
  decoder's state from one version into the next). 233 s, 10 MB, 44 KB a
  second off the disc. Encoding takes a minute or two (a process a
  version); it is redone only when ui.map or this file is newer.

      header    HEADER: 'DCMU', version, rate, version count, the first
                loop's number, block size, the sectors' offset of the data
      versions  VERSION * count: data offset, blocks, samples (a channel)
      data      each version from a 2048-byte sector: blocks of
                BLOCK_SAMPLES samples a channel, the left's ADPCM
                (BLOCK_BYTES / 2) then the right's; the last block padded

    python -m tools.n64.menu_dc <ui.map> --filesystem <file tree>
"""

import argparse
import struct
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from PIL import Image

from . import music
from .bitmaps import bitmap_images, decode
from .hud_dc import DcHudExporter, opaque_texture, sprite_texture
from .sounds_dc import aica_adpcm
from .tagdefs import TagDefinitions
from .xboxmap import XboxMap

SPRITE_LOGO = 0
SPRITE_MAP = 1
SPRITE_GAME_TYPES = 2
# Blood Gulch in mp_map_grafix (n64_menu.c's maps[].picture)
MAP_PICTURE = 9
GAME_TYPE_PICTURES = 5
FONTS = ("ui\\large_ui", "ui\\small_ui")
LOGO = "ui\\shell\\main_menu\\halo_logo"
LOGO_WIDTH = 512
MAP_GRAFIX = "ui\\shell\\bitmaps\\mp_map_grafix"
GAME_TYPE_GRAFIX = "ui\\shell\\bitmaps\\game_type_grafix"
# Halo's main menu's items (ui\shell\main_menu\main_menu_select_list, in
# its order): each a picture of its name, normal and chosen, the widget's
# 256x33 of its 256x64 (dc_menu.c SPRITE_BUTTONS: an item's two in a row)
BUTTONS = ("ui\\shell\\main_menu\\menu_load_campaign", "ui\\shell\\main_menu\\menu_multiplayer",
           "ui\\shell\\main_menu\\menu_settings", "ui\\shell\\main_menu\\menu_game_demos")
BUTTON_SIZE = (256, 33)

MUSIC_MAGIC = b"DCMU"
MUSIC_VERSION = 1
MUSIC_HEADER = struct.Struct("<4sIIHHII8x")
MUSIC_ENTRY = struct.Struct("<III4x")
BLOCK_SAMPLES = 4096
BLOCK_BYTES = BLOCK_SAMPLES  # two channels of 4-bit samples
SECTOR = 2048
assert MUSIC_HEADER.size == 32 and MUSIC_ENTRY.size == 16


def bitmap(xmap, defs, name):
    found = [t for t in xmap.tags if t.group_name == "bitm" and t.name == name]
    if not found:
        raise KeyError(f"{xmap.name} has no {name}")
    return [decode(xmap, image, max(image.width, image.height)).convert("RGBA")
            for image in bitmap_images(xmap, found[0], defs)]


def premultiplied(img):
    """an RGBA image's colour times its alpha (the HUD's blend, dc_hud.c)"""
    r, g, b, a = img.split()
    alpha = a.tobytes()
    return Image.merge("RGBA", tuple(Image.frombytes("L", img.size, bytes(
        (x * y + 127) // 255 for x, y in zip(c.tobytes(), alpha))) for c in (r, g, b)) + (a,))


def pictures(xmap, defs):
    """the sprites in SPRITE_* order: (SPRITE fields before the offset and
    size, data)"""
    sprites = []
    # the logo: what it draws (its alpha's box), LOGO_WIDTH across
    logo = bitmap(xmap, defs, LOGO)[0]
    logo = logo.crop(logo.getchannel("A").getbbox() or (0, 0, logo.width, logo.height))
    logo = logo.resize((LOGO_WIDTH, max(1, round(logo.height * LOGO_WIDTH / logo.width))), Image.LANCZOS)
    kind, tw, th, data = sprite_texture(premultiplied(logo), colour=True)
    sprites.append(((logo.width, logo.height, tw, th, kind, 1, 0), data))
    maps = bitmap(xmap, defs, MAP_GRAFIX)
    types = bitmap(xmap, defs, GAME_TYPE_GRAFIX)
    if len(maps) <= MAP_PICTURE or len(types) < GAME_TYPE_PICTURES:
        raise ValueError(f"{len(maps)} map pictures, {len(types)} game type pictures")
    for img in [maps[MAP_PICTURE]] + types[:GAME_TYPE_PICTURES]:
        # (the picture is the bitmap's top left, ~140x116 of 256x128: the
        # rest black)
        img = img.crop(img.convert("RGB").getbbox() or (0, 0, img.width, img.height))
        kind, tw, th, data = opaque_texture(img)
        sprites.append(((img.width, img.height, tw, th, kind, 1, 0), data))
    for name in BUTTONS:
        states = bitmap(xmap, defs, name)
        if len(states) < 2:
            raise ValueError(f"{name}: {len(states)} pictures, not 2")
        for img in states[:2]:
            img = img.convert("RGBA").crop((0, 0) + BUTTON_SIZE)
            kind, tw, th, data = sprite_texture(premultiplied(img), colour=True)
            sprites.append(((img.width, img.height, tw, th, kind, 1, 0), data))
    return sprites


def write_hud(xmap, defs, filesystem):
    sprites = pictures(xmap, defs)
    fonts = []
    for name in FONTS:
        found = [t for t in xmap.tags if t.group_name == "font" and t.name == name]
        if not found:
            raise KeyError(f"{xmap.name} has no {name}")
        fonts.append(found[0])
    exporter = DcHudExporter(xmap, defs, "ui", filesystem)
    font_table, glyphs, atlas, atlas_size = exporter.font_atlas(fonts)
    exporter.write_file(sprites, font_table, glyphs, atlas, atlas_size, 0)
    vram = sum(len(d) for _, d in sprites) + len(atlas)
    return f"{len(sprites)} pictures, {len(glyphs)} glyphs, {vram // 1024} KB of video memory"


def encode_version(data, channels):
    """one version's Xbox ADPCM as (left ADPCM, right ADPCM, samples)"""
    pcm = music.decode_xbox_adpcm(data, channels)
    samples = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    left = samples[0::channels]
    right = samples[1::channels] if channels == 2 else left
    return aica_adpcm(left), aica_adpcm(right), len(left)


def write_music(xmap, defs, filesystem, source):
    out = Path(filesystem) / "music" / "title.dca"
    newest = max(Path(source).stat().st_mtime, Path(__file__).stat().st_mtime, Path(music.__file__).stat().st_mtime)
    if out.is_file() and out.stat().st_mtime > newest:
        return f"title music as it was ({out.stat().st_size // 1024} KB)"
    lf = {f.name: o for o, f in defs.fields("SoundLooping")}
    tf = {f.name: o for o, f in defs.fields("SoundLoopingTrack")}
    found = [t for t in xmap.tags if t.group_name == "lsnd" and t.name == music.TITLE_MUSIC]
    if not found:
        raise KeyError(f"{xmap.name} has no {music.TITLE_MUSIC}")
    _, pointer = struct.unpack("<iI", xmap.read(found[0].address + lf["tracks"], 8))
    raw = xmap.read(pointer, defs.size("SoundLoopingTrack"))
    start = xmap.tag(struct.unpack_from("<I", raw, tf["start"] + 12)[0])
    loop = xmap.tag(struct.unpack_from("<I", raw, tf["loop"] + 12)[0])
    jobs, rate = [], None
    for tag in (start, loop):
        if tag is None:
            continue
        versions, tag_rate, channels = music.chains(xmap, defs, tag)
        if rate not in (None, tag_rate):
            raise ValueError("the title music's parts differ in rate")
        rate = tag_rate
        jobs += [(data, channels) for data in versions]
    first_loop = 1 if start is not None else 0
    with ProcessPoolExecutor() as pool:
        encoded = list(pool.map(encode_version, *zip(*jobs)))
    table, data = [], bytearray()
    half = BLOCK_BYTES // 2
    for left, right, samples in encoded:
        data.extend(b"\0" * (-len(data) % SECTOR))
        blocks = (samples + BLOCK_SAMPLES - 1) // BLOCK_SAMPLES
        table.append((len(data), blocks, samples))
        for b in range(blocks):
            data += left[b * half:(b + 1) * half].ljust(half, b"\0")
            data += right[b * half:(b + 1) * half].ljust(half, b"\0")
    head = MUSIC_HEADER.size + MUSIC_ENTRY.size * len(table)
    data_offset = (head + SECTOR - 1) // SECTOR * SECTOR
    out_data = bytearray(data_offset)
    MUSIC_HEADER.pack_into(out_data, 0, MUSIC_MAGIC, MUSIC_VERSION, rate, len(table), first_loop, BLOCK_BYTES,
                           data_offset)
    for i, entry in enumerate(table):
        MUSIC_ENTRY.pack_into(out_data, MUSIC_HEADER.size + i * MUSIC_ENTRY.size, *entry)
    out_data += data
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(out_data))
    seconds = [round(samples / rate, 1) for _, _, samples in encoded]
    return f"title music {seconds} s at {rate} Hz, {len(out_data) // 1024} KB"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("map", type=Path)
    parser.add_argument("--filesystem", type=Path, required=True)
    parser.add_argument("--music-only", action="store_true",
                        help="the title music alone (hud/ui.dch is convert.py --dc --ui's: Halo's own menus)")
    args = parser.parse_args()
    xmap = XboxMap(args.map)
    defs = TagDefinitions()
    if not args.music_only:
        print("ui dc menu:", write_hud(xmap, defs, args.filesystem))
    print("ui dc menu:", write_music(xmap, defs, args.filesystem, args.map))


if __name__ == "__main__":
    main()
