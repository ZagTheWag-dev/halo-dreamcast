"""The Dreamcast's HUD images and fonts (hud/<map>.dch, port/dc/src/dc_hud.c),
written by convert.py --dc --filesystem.

The images are tools/n64/hud.py's -- every image of every bitmap a HUD tag
uses, numbered in the same walk order, the number + 1 in the image's
BitmapData 'pixel data offset' (convert.py N64Policy.override reads
sprite_of) -- but for the Dreamcast's 640x480: at their own size (the game
lays its HUD out on 640x480, and the Dreamcast shows it so), as PowerVR
textures, twiddled, kept premultiplied as Halo's HUD blends them (colour +
framebuffer * (1 - alpha): dc_hud.c blends ONE, INVSRCALPHA):

    KIND_COVERAGE  8 bits a texel, palette bank 0 (each value v the
                   premultiplied white v, v, v, v): Halo's ay8 images, and
                   any grey one whose intensity is its alpha
    KIND_GREY      8 bits a texel, palette bank 1 (alpha in the top four
                   bits, intensity in the bottom four): the other grey ones
                   (a8y8), as fine as ARGB4444 at half its size
    KIND_COLOUR    ARGB4444: the rare coloured one
    KIND_METER     the meters at the N64's half size, a byte of fill order
                   and a byte of shape a texel, in RAM: dc_hud.c colours the
                   part a quad shows, as Halo's meter shader would
    KIND_OPAQUE    RGB565, no alpha: the menu's pictures (tools/n64/menu_dc.py
                   writes hud/ui.dch in this file's format)

and the map's fonts (all of them: the game's own text code, draw_string.c,
lays every string out with whichever font it was given, and dc_hud.c draws
its glyphs where it puts them): every glyph's coverage in one 4-bit atlas
(palette bank 32 of the 16-entry banks, white premultiplied). A glyph's
number + 1 goes in its FontCharacter's 'pixels offset' (glyph_of), which is
the Xbox's offset into the font's pixels: those are not in the map the
Dreamcast reads.

File (little-endian; each sprite's data and the atlas 32-aligned):

    header   HEADER: 'DCH1', version, sprite count, font count, glyph
             count, the HUD's font (the HUD globals' "fullscreen font",
             which Halo measures its HUD text with; 0xFFFF none), the
             sections' offsets, the atlas's width and height
    sprites  SPRITE * sprite count: width, height (texels stored),
             texture width, height, kind, scale (texels stored per texel of
             the N64's half-size sprite, which n64_hud_game.c's s and t
             count), flags, data offset, data size
    fonts    FONT * font count: tag name, ascent, descent, leading height
             and width, first glyph, glyph count
    glyphs   GLYPH * glyph count: character, atlas x, y, width, height,
             origin x, y, advance
"""

import struct
from pathlib import Path

from PIL import Image

from .bitmaps import bitmap_images, decode
from .hud import HUD_GROUPS, SKIPPED_NAMES, halve
from .walk import Walker

MAGIC = b"DCH1"
VERSION = 1
HEADER = struct.Struct("<4sIHHHHIIIIHH12x")
SPRITE = struct.Struct("<HHHHBBHII12x")
FONT = struct.Struct("<32shhhhHH4x")
GLYPH = struct.Struct("<HHHBBbbBx")
assert (HEADER.size, SPRITE.size, FONT.size, GLYPH.size) == (48, 32, 48, 12)

KIND_COVERAGE = 0
KIND_GREY = 1
KIND_COLOUR = 2
KIND_METER = 3
KIND_OPAQUE = 4
# VQ compressed (the codebook, 256 2x2 blocks, then a byte a block): the
# shell's large pictures (UiHudExporter)
KIND_VQ_ARGB4444 = 5
KIND_VQ_RGB565 = 6
# a vertical ramp (an image of a colour a row: the shell's gradient, 4x512,
# stretched over the screen): its rows' colours, ARGB8888 premultiplied, a
# long each (dc_hud.c draws it as Gouraud strips: ARGB4444 banded it)
KIND_RAMP = 7
KIND_NAMES = {KIND_COVERAGE: "coverage", KIND_GREY: "grey", KIND_COLOUR: "colour", KIND_METER: "meter",
              KIND_OPAQUE: "opaque", KIND_VQ_ARGB4444: "vq", KIND_VQ_RGB565: "opaque vq", KIND_RAMP: "ramp"}
# (at most this narrow, and this tall, for a ramp)
RAMP_WIDTH = 8
RAMP_HEIGHT = 64


def ramp_rows(img):
    """an RGBA image's rows' colours (each row's average) if every row is
    one colour (within a few steps: the Xbox's compression's noise), else
    None"""
    if img.width > RAMP_WIDTH or img.height < RAMP_HEIGHT:
        return None
    px = img.load()
    rows = []
    for y in range(img.height):
        row = [px[x, y] for x in range(img.width)]
        if any(max(c) - min(c) > 8 for c in zip(*row)):
            return None
        rows.append(tuple((sum(c) + len(c) // 2) // len(c) for c in zip(*row)))
    return rows
# a sprite's flags: loaded into video memory the first time it is drawn
FLAG_STREAMED = 1
MAXIMUM_SPRITES = 512
# the shell's pictures: what the widgets reach (but the 3D's and the HUD's
# own), and the images VQ compressed (at least this many bytes at 16 bits)
UI_ROOTS = ("DeLa", "Soul", "vcky", "matg")
UI_SKIPPED_PREFIXES = ("rasterizer\\", "characters\\", "ui\\mp_map_ui\\", "effects\\", "sky\\", "levels\\",
                       "scenery\\")
VQ_THRESHOLD = 64 * 1024
NO_FONT = 0xFFFF
# the font Halo measures its HUD text with (one local player:
# hud_messaging.c hud_get_font_index), else this one
FONT_FIELD = "fullscreen font"
FONT_FALLBACK = "ui\\small_ui"
# an image counts as coverage when its intensity is within this of its alpha
COVERAGE_SLACK = 3
# a channel within this of the others: grey
GREY_SLACK = 24


def power_of_two(n):
    """the PowerVR's texture side for n texels (8 to 1024)"""
    side = 8
    while side < n:
        side *= 2
    if side > 1024:
        raise ValueError(f"{n} texels: more than the PowerVR's 1024")
    return side


_SPREAD = [sum(((v >> b) & 1) << (2 * b) for b in range(10)) for v in range(1024)]


def twiddled_index(x, y, width, height):
    """a texel's place in a twiddled width x height texture (y on the even
    bits, x on the odd; a rectangle as squares of its smaller side one after
    another: KallistiOS's pvr_txr_load_ex)"""
    side = min(width, height)
    mask = side - 1
    return (_SPREAD[y & mask] | _SPREAD[x & mask] << 1) + (x // side + y // side) * side * side


def twiddle(values, width, height, bits):
    """row-major texel values (width x height, already padded) twiddled, 4,
    8 or 16 bits each"""
    count = width * height
    if bits == 16:
        out = [0] * count
        for y in range(height):
            row = y * width
            for x in range(width):
                out[twiddled_index(x, y, width, height)] = values[row + x]
        return struct.pack(f"<{count}H", *out)
    if bits == 8:
        out = bytearray(count)
        for y in range(height):
            row = y * width
            for x in range(width):
                out[twiddled_index(x, y, width, height)] = values[row + x]
        return bytes(out)
    out = bytearray(count // 2)
    for y in range(height):
        row = y * width
        for x in range(width):
            i = twiddled_index(x, y, width, height)
            out[i >> 1] |= (values[row + x] & 15) << (4 * (i & 1))
    return bytes(out)


def nibble(v):
    return (v * 15 + 127) // 255


def padded(img, width, height):
    """img in the top left of a width x height image, the rest transparent"""
    if img.size == (width, height):
        return img
    out = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    out.paste(img, (0, 0))
    return out


def sprite_texture(img, colour=False):
    """an RGBA image (premultiplied) as (kind, texture width, height, data);
    with colour, ARGB4444 whatever its colours"""
    tw, th = power_of_two(img.width), power_of_two(img.height)
    r, g, b, a = img.split()
    grey = not colour and max(abs(x - y) for x, y in zip(r.getextrema(), g.getextrema())) < GREY_SLACK and \
        max(abs(x - y) for x, y in zip(g.getextrema(), b.getextrema())) < GREY_SLACK
    full = padded(img, tw, th)
    if grey:
        y = full.convert("RGB").convert("L").tobytes()
        alpha = full.getchannel("A").tobytes()
        if all(abs(i - k) <= COVERAGE_SLACK for i, k in zip(y, alpha)):
            return KIND_COVERAGE, tw, th, twiddle(alpha, tw, th, 8)
        return KIND_GREY, tw, th, twiddle([nibble(k) << 4 | nibble(i) for i, k in zip(y, alpha)], tw, th, 8)
    px = full.tobytes()
    values = [nibble(px[i + 3]) << 12 | nibble(px[i]) << 8 | nibble(px[i + 1]) << 4 | nibble(px[i + 2])
              for i in range(0, len(px), 4)]
    return KIND_COLOUR, tw, th, twiddle(values, tw, th, 16)


def opaque_texture(img):
    """an RGB image as (KIND_OPAQUE, texture width, height, data): RGB565,
    twiddled, the padding black"""
    tw, th = power_of_two(img.width), power_of_two(img.height)
    full = Image.new("RGB", (tw, th))
    full.paste(img.convert("RGB"), (0, 0))
    px = full.tobytes()
    values = [(px[i] >> 3) << 11 | (px[i + 1] >> 2) << 5 | px[i + 2] >> 3 for i in range(0, len(px), 3)]
    return KIND_OPAQUE, tw, th, twiddle(values, tw, th, 16)


def meter_bytes(img):
    """a meter's fill order (its grey) and shape (its alpha), a byte each"""
    order = img.convert("RGB").convert("L").tobytes()
    out = bytearray(len(order) * 2)
    out[0::2] = order
    out[1::2] = img.getchannel("A").tobytes()
    return bytes(out)


class DcHudExporter:
    """hud.py's images and numbering, and the map's fonts, written for the
    Dreamcast. Set as the policy's hud (convert.py) and collected before
    the repack: every HUD image's pixel data offset becomes its sprite's
    number + 1 (sprite_of), every font character's pixels offset its
    glyph's (glyph_of); 0 for the rest"""

    def __init__(self, xmap, defs, map_name, out_dir):
        self.map = xmap
        self.defs = defs
        self.map_name = map_name
        self.out_dir = Path(out_dir)
        # the Xbox address of an image's bitmap data -> sprite number
        self.sprite_of = {}
        # the Xbox address of a font character -> glyph number
        self.glyph_of = {}
        self.count = 0

    def collect(self):
        bitmaps, hud_fonts = [], []
        walker = Walker(self.map, self.defs)

        def on_dependency(tag, address, f, path, group, datum):
            t = self.map.tag(datum)
            if t is None:
                return
            if t.group_name == "bitm" and t not in bitmaps:
                bitmaps.append(t)
            elif t.group_name == "font" and getattr(f, "name", "") == FONT_FIELD and t not in hud_fonts:
                hud_fonts.append(t)

        for tag in self.map.tags:
            if tag.group_name in HUD_GROUPS:
                walker.walk(tag, on_dependency=on_dependency)
        bf = {fl.name: o for o, fl in self.defs.fields("Bitmap")}
        size = self.defs.size("BitmapData")
        sprites = []  # (SPRITE fields before the offset and size, data)
        kinds = [0, 0, 0, 0]
        for tag in bitmaps:
            if any(name in tag.name for name in SKIPPED_NAMES):
                continue
            count, pointer = struct.unpack("<iI", self.map.read(tag.address + bf["bitmap data"], 8))
            for i, image in enumerate(bitmap_images(self.map, tag, self.defs)):
                if image.type != 0:  # 2D images only
                    continue
                try:
                    img = decode(self.map, image, max(image.width, image.height)).convert("RGBA")
                except ValueError:
                    continue
                if "meter" in tag.name:
                    img = halve(img, False)
                    sprites.append(((img.width, img.height, img.width, img.height, KIND_METER, 1, 0),
                                    meter_bytes(img)))
                    kinds[KIND_METER] += 1
                else:
                    kind, tw, th, data = sprite_texture(img)
                    sprites.append(((img.width, img.height, tw, th, kind, 2, 0), data))
                    kinds[kind] += 1
                self.sprite_of[pointer + i * size] = len(sprites) - 1
        if len(sprites) > MAXIMUM_SPRITES:
            raise ValueError(f"{len(sprites)} HUD images: dc_hud.c takes {MAXIMUM_SPRITES}")
        self.count = len(sprites)
        fonts = [t for t in self.map.tags if t.group_name == "font"]
        hud_font = hud_fonts[0] if hud_fonts else next((t for t in fonts if t.name == FONT_FALLBACK), None)
        font_table, glyphs, atlas, atlas_size = self.font_atlas(fonts)
        hud_index = fonts.index(hud_font) if hud_font in fonts else NO_FONT
        self.write_file(sprites, font_table, glyphs, atlas, atlas_size, hud_index)
        vram = sum(len(d) for f, d in sprites if f[4] != KIND_METER) + len(atlas)
        print(f"{self.map_name} dc hud: {self.count} images ({kinds[KIND_COVERAGE]} coverage, {kinds[KIND_GREY]} grey, "
              f"{kinds[KIND_COLOUR]} colour, {kinds[KIND_METER]} meters), {len(fonts)} fonts, {len(glyphs)} glyphs "
              f"in {atlas_size[0]}x{atlas_size[1]}, the HUD's font {hud_font.name if hud_font else None}; "
              f"{vram // 1024} KB of video memory")
        return self.count

    def font_atlas(self, fonts):
        """every font's glyphs (tools/n64/ui.py export_font's) on shelves,
        tallest first: (font table, glyph table, 4-bit atlas, its size)"""
        f = {fl.name: o for o, fl in self.defs.fields("Font")}
        cf = {fl.name: o for o, fl in self.defs.fields("FontCharacter")}
        csize = self.defs.size("FontCharacter")
        table, glyphs = [], []
        for tag in fonts:
            ascent, descent, leading_height, leading_width = struct.unpack(
                "<hhhh", self.map.read(tag.address + f["ascending height"], 8))
            count, pointer = struct.unpack("<iI", self.map.read(tag.address + f["characters"], 8))
            psize, _, _, ppointer, _ = struct.unpack("<iIIII", self.map.read(tag.address + f["pixels"], 20))
            pixels = self.map.read(ppointer, psize) if psize > 0 else b""
            first = len(glyphs)
            for i in range(max(count, 0)):
                address = pointer + i * csize
                raw = self.map.read(address, csize)
                code, advance, width, height, ox, oy = struct.unpack_from("<Hhhhhh", raw, 0)
                offset = struct.unpack_from("<I", raw, cf["pixels offset"])[0]
                width, height = max(width, 0), max(height, 0)
                data = pixels[offset:offset + width * height] if width and height else b""
                if len(data) != width * height or width > 255 or height > 255:
                    width = height = 0
                    data = b""
                self.glyph_of[address] = len(glyphs)
                glyphs.append([code, 0, 0, width, height, max(-128, min(127, ox)), max(-128, min(127, oy)),
                               max(0, min(255, advance)), data])
            name = tag.name.encode()[:31]
            table.append(FONT.pack(name, ascent, descent, leading_height, leading_width, first, len(glyphs) - first))
        if len(glyphs) > 0xFFFF:
            raise ValueError(f"{len(glyphs)} glyphs")
        # shelves, the tallest glyphs first, as narrow an atlas as holds them
        # in 512 rows
        order = sorted((g for g in glyphs if g[3] and g[4]), key=lambda g: (-g[4], -g[3]))
        for atlas_width in (256, 512, 1024):
            x = y = shelf = 0
            for g in order:
                if x + g[3] + 1 > atlas_width:
                    x, y, shelf = 0, y + shelf + 1, 0
                g[1], g[2] = x, y
                x += g[3] + 1
                shelf = max(shelf, g[4])
            if y + shelf <= 512 or atlas_width == 1024:
                break
        atlas_height = power_of_two(y + shelf + 1)
        coverage = bytearray(atlas_width * atlas_height)
        for g in order:
            for row in range(g[4]):
                start = (g[2] + row) * atlas_width + g[1]
                coverage[start:start + g[3]] = bytes(nibble(v) for v in g[8][row * g[3]:(row + 1) * g[3]])
        atlas = twiddle(coverage, atlas_width, atlas_height, 4)
        return table, [GLYPH.pack(*g[:8]) for g in glyphs], atlas, (atlas_width, atlas_height)

    def write_file(self, sprites, fonts, glyphs, atlas, atlas_size, hud_font):
        sprites_offset = HEADER.size
        fonts_offset = sprites_offset + SPRITE.size * len(sprites)
        glyphs_offset = fonts_offset + FONT.size * len(fonts)
        out = bytearray(glyphs_offset)
        out += b"".join(glyphs)
        for n, font in enumerate(fonts):
            out[fonts_offset + n * FONT.size:fonts_offset + (n + 1) * FONT.size] = font

        def place(data):
            out.extend(b"\0" * (-len(out) % 32))
            offset = len(out)
            out.extend(data)
            return offset

        for n, (fields, data) in enumerate(sprites):
            offset = place(data)
            SPRITE.pack_into(out, sprites_offset + n * SPRITE.size, *fields, offset, len(data))
        atlas_offset = place(atlas)
        HEADER.pack_into(out, 0, MAGIC, VERSION, len(sprites), len(fonts), len(glyphs), hud_font,
                         sprites_offset, fonts_offset, glyphs_offset, atlas_offset, *atlas_size)
        path = self.out_dir / "hud" / f"{self.map_name}.dch"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(bytes(out))


def premultiplied(img):
    """a straight RGBA image premultiplied by its alpha"""
    r, g, b, a = img.split()
    black = Image.new("L", img.size, 0)
    return Image.merge("RGBA", [Image.composite(c, black, a) for c in (r, g, b)] + [a]) if a.getextrema()[0] < 255 \
        else img


def vq_texture(img, opaque, cache_dir):
    """an RGBA power-of-two image VQ compressed (pvrtex, no mipmaps:
    rectangles allowed) as (kind, data: the codebook, then the indices),
    RGB565 if opaque, else ARGB4444; kept by content in cache_dir"""
    import hashlib
    import os
    import subprocess
    import tempfile
    from .level_dc import PVRTEX, PVRTEX_PATH
    fmt = "RGB565" if opaque else "ARGB4444"
    key = hashlib.sha1(b"ui vq 1" + fmt.encode() + struct.pack("<HH", *img.size) + img.tobytes()).hexdigest()
    cached = Path(cache_dir) / f"{key}.vq"
    if cached.exists():
        data = cached.read_bytes()
    else:
        with tempfile.TemporaryDirectory() as work:
            png = os.path.join(work, "in.png")
            dt = os.path.join(work, "out.dt")
            img.save(png)
            env = dict(os.environ)
            env["PATH"] = PVRTEX_PATH + os.pathsep + env.get("PATH", "")
            done = subprocess.run([PVRTEX, "-i", png, "-o", dt, "-f", fmt, "-c"], capture_output=True, text=True,
                                  env=env)
            if done.returncode:
                raise RuntimeError(f"pvrtex failed on a {img.size} image: {done.stdout[-400:]} {done.stderr[-400:]}")
            raw = open(dt, "rb").read()
        if raw[:4] != b"DcTx":
            raise ValueError("pvrtex wrote no .dt texture")
        data = raw[(raw[9] + 1) * 32:struct.unpack_from("<I", raw, 4)[0]]
        cached.parent.mkdir(parents=True, exist_ok=True)
        cached.write_bytes(data)
    expected = 2048 + img.width * img.height // 4
    if len(data) != expected:
        raise ValueError(f"a {img.size} VQ texture of {len(data)} bytes, not {expected}")
    return (KIND_VQ_RGB565 if opaque else KIND_VQ_ARGB4444), data


class UiHudExporter(DcHudExporter):
    """ui.map's widget pictures and its fonts (convert.py --dc --ui):
    every image of every bitmap a widget, a widget collection, the virtual
    keyboard or the globals' interface reach, premultiplied (the widgets
    blend straight: n64_hud_game.c premultiplies their colour), streamed
    (FLAG_STREAMED: dc_hud.c loads each into video memory the first time it
    is drawn, and gives back the least lately drawn when it needs room: the
    shell's pictures are ~28 MB at 16 bits a texel), the large ones VQ
    compressed"""

    def collect(self):
        bitmaps = []
        walker = Walker(self.map, self.defs)

        def on_dependency(tag, address, f, path, group, datum):
            t = self.map.tag(datum)
            if t is not None and t.group_name == "bitm" and t not in bitmaps and \
                    not t.name.startswith(UI_SKIPPED_PREFIXES):
                bitmaps.append(t)

        for tag in self.map.tags:
            if tag.group_name in UI_ROOTS:
                walker.walk(tag, on_dependency=on_dependency)
        bf = {fl.name: o for o, fl in self.defs.fields("Bitmap")}
        size = self.defs.size("BitmapData")
        cache = self.out_dir.parent / "vq-cache"
        sprites = []
        kinds = {}
        sixteen = 0
        for tag in bitmaps:
            count, pointer = struct.unpack("<iI", self.map.read(tag.address + bf["bitmap data"], 8))
            for i, image in enumerate(bitmap_images(self.map, tag, self.defs)):
                if image.type != 0:
                    continue
                try:
                    img = decode(self.map, image, max(image.width, image.height)).convert("RGBA")
                except ValueError:
                    continue
                tw, th = power_of_two(img.width), power_of_two(img.height)
                sixteen += tw * th * 2
                opaque = img.getchannel("A").getextrema()[0] == 255
                img = premultiplied(img)
                rows = ramp_rows(img)
                if rows is not None:
                    kind, tw, th = KIND_RAMP, img.width, img.height
                    data = struct.pack(f"<{len(rows)}I", *((a << 24) | (r << 16) | (g << 8) | b for r, g, b, a in rows))
                elif tw * th * 2 >= VQ_THRESHOLD and min(tw, th) >= 8:
                    kind, data = vq_texture(padded(img, tw, th), opaque, cache)
                elif opaque:
                    kind, tw, th, data = opaque_texture(img)
                else:
                    kind, tw, th, data = sprite_texture(img)
                kinds[kind] = kinds.get(kind, 0) + 1
                # (a ramp is in RAM from the start: no texture)
                sprites.append(((img.width, img.height, tw, th, kind, 2, 0 if kind == KIND_RAMP else FLAG_STREAMED),
                                data))
                self.sprite_of[pointer + i * size] = len(sprites) - 1
        if len(sprites) > MAXIMUM_SPRITES:
            raise ValueError(f"{len(sprites)} widget images: dc_hud.c takes {MAXIMUM_SPRITES}")
        self.count = len(sprites)
        fonts = [t for t in self.map.tags if t.group_name == "font"]
        hud_font = next((t for t in fonts if t.name == FONT_FALLBACK), None)
        font_table, glyphs, atlas, atlas_size = self.font_atlas(fonts)
        self.write_file(sprites, font_table, glyphs, atlas, atlas_size, fonts.index(hud_font) if hud_font else NO_FONT)
        stored = sum(len(d) for f, d in sprites)
        print(f"{self.map_name} dc widgets: {len(bitmaps)} bitmaps, {self.count} images ("
              + ", ".join(f"{KIND_NAMES[k]} {n}" for k, n in sorted(kinds.items()))
              + f"), {stored // 1024} KB streamed ({sixteen // 1024} KB at 16 bits), {len(fonts)} fonts, "
              f"{len(glyphs)} glyphs in {atlas_size[0]}x{atlas_size[1]}")
        return self.count


class NoHudSprites:
    """convert.py --dc without a file tree: no sprites and no glyphs, so every
    bitmap's pixel data offset and every font character's pixels offset is
    0 (the Xbox's own offsets would be read as numbers)"""
    sprite_of = {}
    glyph_of = {}
