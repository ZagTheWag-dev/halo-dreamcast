"""The Dreamcast's level (levels/<map>.dcl, port/dc/src/dc_level.c): the
N64's level geometry, light and PVS groups (level.py LevelBuilder: build,
batches, triangle_strips), written little-endian as indexed strip runs for
the PowerVR, floats throughout (the SH-4 transforms float vectors).

A run is one material's triangles, at most RUN_VERTICES vertices (the
batches the N64 builds, without its texture-coordinate limit: the PowerVR
takes float coordinates); a group is one of the N64's PVS groups, drawn
when any of its owner clusters is visible. Each vertex carries its light;
the PowerVR multiplies its material's texture by it (the textures doubled,
as Halo's environment shaders double texture x light, level.py).

Textures (write_texture): square, a power of two up to TEXTURE_SIZE a
side, twiddled, with every mipmap (KallistiOS's pvrtex layout: the 1x1
level at byte 6, each larger one after it), 16 bits a texel: RGB565, or
ARGB1555 for a cut-out (alpha-tested) texture. Natural terrain (an
environment shader repeating over TERRAIN_REPEAT_UNITS or more, with
detail maps: Blood Gulch's ground) is its base map at TERRAIN_SIZE with
its detail maps' average effect on each texel (level.py detail_tint, as
the N64's tiles take it), not doubled: Halo's detail maps double.

File layout (all little-endian, every section 4-aligned, the vertex
sections 32-aligned):

    header      HEADER
    textures    TEXTURE * texture_count
    materials   MATERIAL * material_count
    groups      GROUP * group_count
    owners      u16 * owner_count (BSP cluster indices)
    runs        RUN * run_count
    positions   4f * vertex_count (x, y, z, 0: world units)
    attributes  2f I * vertex_count (u, v recentred per run; 0xFFRRGGBB light)
    indices     u16 * index_count (run-local; bit 15 on each strip's last)
    texdata     the textures' PowerVR data (R2; empty in R1)
"""

import math
import os
import struct
import subprocess
import tempfile
from contextlib import contextmanager

from . import level as L
from .level import LevelBuilder, triangle_strips

DCL_MAGIC = b"DCL1"
DCL_VERSION = 1
RUN_VERTICES = 64
STRIP_MAX = 256
TEXTURE_SIZE = 256
TERRAIN_SIZE = 512

# dcl_texture.format (port/dc/src/dc_level.c)
FORMAT_RGB565 = 0
FORMAT_ARGB1555 = 1
FORMAT_ARGB4444 = 2
# (with the format: VQ compressed, KallistiOS's pvrtex .dt data: the
# codebook, then the indices of every mipmap)
FORMAT_VQ = 0x100
TEXTURE_MIPMAPPED = 1

# KallistiOS's texture encoder (kos/utils/pvrtex, built with DreamSDK's
# MinGW: its DLLs beside it in mingw64/bin)
PVRTEX = os.environ.get("PVRTEX", "C:/DreamSDK/opt/toolchains/dc/kos/utils/pvrtex/pvrtex.exe")
PVRTEX_PATH = os.environ.get("PVRTEX_PATH", "C:/DreamSDK/mingw64/bin")
PVRTEX_FORMATS = {FORMAT_RGB565: "RGB565", FORMAT_ARGB1555: "ARGB1555", FORMAT_ARGB4444: "ARGB4444"}

# a 16-bit twiddled mipmap chain: each level's offset (pvrtex MipMapOffset),
# the chain's size the next level's offset
MIPMAP_OFFSETS = (0x6, 0x8, 0x10, 0x30, 0xB0, 0x2B0, 0xAB0, 0x2AB0, 0xAAB0, 0x2AAB0, 0xAAAB0)

HEADER = struct.Struct("<4sI4HIII9II6fI")
TEXTURE = struct.Struct("<IIHHIII2I")
MATERIAL = struct.Struct("<hHI2I")
GROUP = struct.Struct("<6f4H")
RUN = struct.Struct("<HHIII6f")

assert (HEADER.size, TEXTURE.size, MATERIAL.size, GROUP.size, RUN.size) == (96, 32, 16, 32, 40)


@contextmanager
def run_limits(vertices):
    """level.py's batch limits for the Dreamcast's runs (batches reads them
    when it is called)"""
    saved = L.MAXIMUM_BATCH_VERTICES, L.MAXIMUM_UV_EXTENT
    L.MAXIMUM_BATCH_VERTICES, L.MAXIMUM_UV_EXTENT = vertices, math.inf
    try:
        yield
    finally:
        L.MAXIMUM_BATCH_VERTICES, L.MAXIMUM_UV_EXTENT = saved


@contextmanager
def decode_size(texels, *modules):
    """level.py's (and the other modules') textures decoded from mipmaps of
    at least `texels` a side (the N64 decodes at 64)"""
    modules = (L,) + modules
    originals = [m.decode for m in modules]

    def wrap(original):
        def decode(xmap, image, target=64):
            return original(xmap, image, max(target, texels))
        return decode

    for m, original in zip(modules, originals):
        m.decode = wrap(original)
    try:
        yield
    finally:
        for m, original in zip(modules, originals):
            m.decode = original


def twiddle_table(n):
    """the twiddled index of each texel (x on the odd bits, y on the even:
    pvrtex Morton2DInit) of an n x n level, by y * n + x"""
    bits = n.bit_length() - 1
    table = []
    for y in range(n):
        for x in range(n):
            index = 0
            for b in range(bits):
                index |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1)
            table.append(index)
    return table


def pack_texel(r, g, b, a, fmt):
    if fmt == FORMAT_RGB565:
        return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
    if fmt == FORMAT_ARGB1555:
        return (0x8000 if a >= 128 else 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3
    return (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4


def mipmap_chain(img, fmt):
    """an RGBA square power-of-two image as a twiddled 16-bit mipmap chain"""
    from PIL import Image

    n = img.width
    levels = n.bit_length() - 1
    out = bytearray(MIPMAP_OFFSETS[levels + 1])
    for level in range(levels + 1):
        size = 1 << level
        mip = img if size == n else img.resize((size, size), Image.BOX)
        pixels = list(mip.getdata())
        texels = [0] * (size * size)
        for i, t in enumerate(twiddle_table(size)):
            texels[t] = pack_texel(*pixels[i], fmt)
        struct.pack_into(f"<{size * size}H", out, MIPMAP_OFFSETS[level], *texels)
    return bytes(out)


def vq_chain(img, fmt):
    """an RGBA square power-of-two image VQ compressed with every mipmap
    (pvrtex: a .dt file, its data after the header)"""
    with tempfile.TemporaryDirectory() as work:
        png = os.path.join(work, "in.png")
        dt = os.path.join(work, "out.dt")
        img.save(png)
        env = dict(os.environ)
        env["PATH"] = PVRTEX_PATH + os.pathsep + env.get("PATH", "")
        done = subprocess.run([PVRTEX, "-i", png, "-o", dt, "-f", PVRTEX_FORMATS[fmt], "-c", "-m"],
                              capture_output=True, text=True, env=env)
        if done.returncode:
            raise RuntimeError(f"pvrtex failed on a {img.size} texture: {done.stdout[-400:]} {done.stderr[-400:]}")
        data = open(dt, "rb").read()
    if data[:4] != b"DcTx":
        raise ValueError("pvrtex wrote no .dt texture")
    size = struct.unpack_from("<I", data, 4)[0]
    return data[(data[9] + 1) * 32:size]


def argb(rgb, alpha=255):
    r, g, b = (max(0, min(255, int(round(c * 255)))) for c in rgb)
    return (alpha << 24) | (r << 16) | (g << 8) | b


# a terrain's detail map's side (texels: dc_level.c draws it near the eye)
DETAIL_TEXTURE_SIZE = 128


def detail_step(scale):
    """the fewest whole base tiles that are whole detail tiles too (at
    most 16; 1 if none)"""
    for m in range(1, 17):
        if abs(m * scale - round(m * scale)) < 1e-3:
            return m
    return 1


class DcLevelBuilder(LevelBuilder):
    def __init__(self, xmap, defs, map_name, out_dir):
        super().__init__(xmap, defs, map_name, out_dir, split_depth=2)
        # (the N64's texture tricks off: full-colour textures from R2)
        self.greyscale = self.terrain_detail = self.overlay_detail = self.colour_tiles = False
        # (chicago shaders named as water drawn as water: a30's rivers and
        # waterfalls, blended on the translucent list, dc_level.c)
        self.chicago_water = True
        # each texture's PowerVR data, format and size (write_texture)
        self.texture_data = []
        # the largest a texture may be (models_dc.py: its own)
        self.texture_limit = TEXTURE_SIZE
        # textures VQ compressed (pvrtex: an eighth of the video memory; the
        # tile accelerator's vertex buffer needs it, dc_render.c), and a
        # change-colour texture's overlay: texture index -> its overlay's
        self.vq = True
        self.texture_overlay = {}
        # natural terrain's detail (terrain_texture): its base texture ->
        # (its primary detail map's texture, the detail's scale)
        self.texture_detail = {}
        # a texture brightened less than asked (write_texture): the factor
        # its materials' light makes up
        self.texture_compensation = {}
        # textures whose detail is drawn nearer the eye than terrain's (not
        # terrain: cliffs, walls; written as a negative scale, dc_level.c)
        self.near_detail = set()

    def write_texture(self, img, tag_name, mask=None, fmt=None, cutout=False, limit=None):
        """level.py's texture, for the PowerVR: square, doubled, a mipmap
        chain; with a mask (L: a player's colour change) also its overlay,
        the texture with the mask as its alpha (ARGB4444), which the
        renderer draws over the texture tinted by the player's colour"""
        from PIL import Image

        side = max(img.size)
        n = max(8, min(limit or self.texture_limit, 1 << (side.bit_length() - 1)))
        img = img.convert("RGBA").resize((n, n), Image.LANCZOS)
        requested = k = self.texture_brightness
        if requested > 1.0:
            # (only the gain the texture takes without its bright texels
            # clipping -- its 99th percentile's brightest channel at most
            # white -- the rest on its materials' light: add_material. a30's
            # cliff rock, doubled whole, washed out to white streaks)
            from PIL import ImageChops
            r, g, b, _ = img.split()
            histogram = ImageChops.lighter(ImageChops.lighter(r, g), b).histogram()
            total, seen, p99 = n * n, 0, 255
            for value in range(255, -1, -1):
                seen += histogram[value]
                if seen > total // 100:
                    p99 = value
                    break
            k = max(1.0, min(requested, 255.0 / max(1, p99)))
        if k != 1.0:
            r, g, b, a = img.split()
            r, g, b = (ch.point(lambda v: min(255, int(v * k))) for ch in (r, g, b))
            img = Image.merge("RGBA", (r, g, b, a))
        texture_format = FORMAT_ARGB4444 if fmt == "ARGB4444" else FORMAT_ARGB1555 if cutout else FORMAT_RGB565
        if self.vq:
            self.texture_data.append((vq_chain(img, texture_format), texture_format | FORMAT_VQ))
        else:
            self.texture_data.append((mipmap_chain(img, texture_format), texture_format))
        rgb = img.convert("RGB")
        alpha = list(img.getchannel("A").getdata())
        pixels = [p for i, p in enumerate(rgb.getdata()) if not cutout or alpha[i] >= 128] or list(rgb.getdata())
        colour = tuple(sum(p[k] for p in pixels) / len(pixels) / 255.0 for k in range(3))
        self.textures.append(L.Texture(f"{self.map_name}-{len(self.textures)}", n, n, colour))
        index = len(self.textures) - 1
        if requested != k:
            self.texture_compensation[index] = requested / k
        if mask is not None:
            overlay = img.copy()
            alpha = mask.convert("L").resize((n, n), Image.LANCZOS)
            if cutout:
                # (the overlay only where the texture is not cut out)
                alpha = Image.composite(alpha, Image.new("L", (n, n), 0), img.getchannel("A").point(lambda a: 255 if a >= 128 else 0))
            overlay.putalpha(alpha)
            if self.vq:
                self.texture_data.append((vq_chain(overlay, FORMAT_ARGB4444), FORMAT_ARGB4444 | FORMAT_VQ))
            else:
                self.texture_data.append((mipmap_chain(overlay, FORMAT_ARGB4444), FORMAT_ARGB4444))
            self.textures.append(L.Texture(f"{self.map_name}-{len(self.textures)}", n, n, colour))
            self.texture_overlay[index] = len(self.textures) - 1
        return index

    def add_material(self, texture, flags, shade=1.0, **kwargs):
        return super().add_material(texture, flags, shade * self.texture_compensation.get(texture, 1.0), **kwargs)

    def material(self, shader_datum):
        if shader_datum not in self.material_by_shader:
            texture = self.terrain_texture(shader_datum)
            if texture >= 0:
                self.material_by_shader[shader_datum] = self.add_material(texture, 0)
        index = super().material(shader_datum)
        # (any other environment shader with a detail map gets the detail
        # pass too: a30's cliffs, whose 64-texel-high base map is a blur
        # close up without its rock grain)
        texture, flags = self.materials[index]
        tag = self.map.tag(shader_datum)
        if texture >= 0 and texture not in self.texture_detail and not flags & L.MATERIAL_ALPHA_TEST and \
                tag is not None and tag.group_name == "senv":
            brightness, self.texture_brightness = self.texture_brightness, 1.0
            try:
                detail = self.detail_texture(tag, self.fields("ShaderEnvironment"))
            finally:
                self.texture_brightness = brightness
            if detail is not None:
                self.texture_detail[texture] = detail
                self.near_detail.add(texture)
        return index

    def terrain_texture(self, shader_datum):
        """natural terrain's texture (the module's comment), or -1: not
        terrain"""
        from PIL import Image

        tag = self.map.tag(shader_datum)
        if tag is None or tag.group_name != "senv":
            return -1
        f = self.fields("ShaderEnvironment")
        read = lambda field, fmt: struct.unpack(fmt, self.map.read(tag.address + f[field], struct.calcsize(fmt)))[0]
        if read("shader environment flags", "<H") & 1 or self.shader_span.get(shader_datum, 0.0) < L.TERRAIN_REPEAT_UNITS:
            return -1
        base = self.map.tag(read("base map", "<12xI"))
        images = L.bitmap_images(self.map, base, self.defs) if base is not None else []
        if not images or all(read(field, "<12xI") == 0xFFFFFFFF
                             for field in ("primary detail map", "secondary detail map")):
            return -1
        img = L.decode(self.map, images[0], TERRAIN_SIZE).convert("RGBA")
        terrain = L.Terrain(None, [])
        terrain.tint = self.detail_tint(tag, read("shader environment type", "<H") in (1, 2))
        pixels = []
        for p in img.getdata():
            colour = terrain.tinted([c / 255.0 for c in p])
            pixels.append(tuple(max(0, min(255, int(round(c * 255)))) for c in colour) + (255,))
        tinted = Image.new("RGBA", img.size)
        tinted.putdata(pixels)
        brightness, self.texture_brightness = self.texture_brightness, 1.0
        try:
            texture = self.write_texture(tinted, base.name, limit=TERRAIN_SIZE)
            detail = self.detail_texture(tag, f)
            if detail is not None:
                self.texture_detail[texture] = detail
            return texture
        finally:
            self.texture_brightness = brightness

    def detail_texture(self, tag, f):
        """a terrain's primary detail map as a texture of its own, its mean
        grey (the base map carries the detail's average effect: detail_tint),
        drawn near the eye over the base, doubled (dc_level.c's detail pass:
        twice the frame times it, Halo's detail maps' double multiply);
        (texture, scale) or None"""
        from PIL import Image, ImageStat

        datum = struct.unpack("<I", self.map.read(tag.address + f["primary detail map"] + 12, 4))[0]
        scale = struct.unpack("<f", self.map.read(tag.address + f["primary detail map scale"], 4))[0]
        detail = self.map.tag(datum)
        images = L.bitmap_images(self.map, detail, self.defs) if detail is not None else []
        if not images or scale <= 0.0:
            return None
        try:
            img = L.decode(self.map, images[0], DETAIL_TEXTURE_SIZE).convert("RGB")
        except ValueError:
            return None
        means = ImageStat.Stat(img).mean
        img = Image.merge("RGB", [band.point(lambda v, k=128.0 / max(1.0, m): max(0, min(255, int(v * k))))
                                  for band, m in zip(img.split(), means)])
        return self.write_texture(img.convert("RGBA"), detail.name, limit=DETAIL_TEXTURE_SIZE), scale

    def write(self, path, bsp_tag):
        # (each shader's repeat, for terrain_texture: level.py measures it
        # only for its own terrain detail)
        self.shader_span = self.shader_spans(bsp_tag)
        with decode_size(TEXTURE_SIZE):
            clusters, surface_info = self.build(bsp_tag)
        groups, owners, runs = [], [], []
        positions, attributes, indices = bytearray(), bytearray(), bytearray()
        vertex_count = index_count = triangle_count = 0
        with run_limits(RUN_VERTICES):
            for surfaces, owned in clusters:
                by_material = {}
                for s in surfaces:
                    for info in surface_info[s] or ():
                        by_material.setdefault(info[0], []).append(info)
                first_run = len(runs)
                group_lo, group_hi = [math.inf] * 3, [-math.inf] * 3
                for material in sorted(by_material):
                    shade = self.material_shade[material]
                    su, sv = self.material_uv_scale[material]
                    # (a detailed terrain's runs recentred by whole tiles of
                    # its detail too: no seams between runs)
                    step = detail_step(self.texture_detail.get(self.materials[material][0], (0, 1.0))[1])
                    for _, verts, tris in self.batches(by_material[material]):
                        u0 = math.floor(min(v[2][0] * su for v in verts) / step) * step
                        v0 = math.floor(min(v[2][1] * sv for v in verts) / step) * step
                        lo = [min(v[0][k] for v in verts) for k in range(3)]
                        hi = [max(v[0][k] for v in verts) for k in range(3)]
                        for v in verts:
                            positions += struct.pack("<4f", *v[0], 0.0)
                            attributes += struct.pack("<2fI", v[2][0] * su - u0, v[2][1] * sv - v0,
                                                      argb([c * shade for c in v[3]]))
                        first_index = index_count
                        for strip in triangle_strips(tris):
                            if len(strip) > STRIP_MAX:
                                raise ValueError(f"a strip of {len(strip)} indices")
                            indices += struct.pack(f"<{len(strip)}H", *strip[:-1], strip[-1] | 0x8000)
                            index_count += len(strip)
                        runs.append((material, len(verts), vertex_count, first_index, index_count - first_index,
                                     lo, hi))
                        vertex_count += len(verts)
                        triangle_count += len(tris)
                        group_lo = [min(a, b) for a, b in zip(group_lo, lo)]
                        group_hi = [max(a, b) for a, b in zip(group_hi, hi)]
                if group_lo[0] == math.inf:
                    group_lo = group_hi = [0.0] * 3
                groups.append((group_lo, group_hi, first_run, len(runs) - first_run, len(owners), len(owned)))
                owners.extend(owned)

        materials = bytearray()
        for m, (texture, flags) in enumerate(self.materials):
            if m in self.water_colour:
                flat = argb(self.water_colour[m], 128)
            elif texture >= 0:
                flat = argb(self.textures[texture].colour)
            else:
                flat = argb((0.5, 0.5, 0.5))
            detail, detail_scale = self.texture_detail.get(texture, (-1, 0.0))
            if texture in self.near_detail:
                detail_scale = -detail_scale
            materials += MATERIAL.pack(texture, flags, flat, detail + 1, struct.unpack("<I", struct.pack("<f", detail_scale))[0])
        texdata = bytearray()
        textures = bytearray()
        for t, (data, texture_format) in zip(self.textures, self.texture_data):
            texdata.extend(b"\0" * (-len(texdata) % 32))
            textures += TEXTURE.pack(len(texdata), len(data), t.width, t.height, texture_format, TEXTURE_MIPMAPPED,
                                     argb(t.colour), 0, 0)
            texdata += data

        out = bytearray(HEADER.size)

        def section(data, align=4):
            out.extend(b"\0" * (-len(out) % align))
            offset = len(out)
            out.extend(data)
            return offset

        o_textures = section(textures)
        o_materials = section(materials)
        o_groups = section(b"".join(GROUP.pack(*g[0], *g[1], *g[2:]) for g in groups))
        o_owners = section(struct.pack(f"<{len(owners)}H", *owners))
        o_runs = section(b"".join(RUN.pack(*r[:5], *r[5], *r[6]) for r in runs))
        o_positions = section(positions, 32)
        o_attributes = section(attributes, 32)
        o_indices = section(indices)
        o_texdata = section(texdata, 32)
        out.extend(b"\0" * (-len(out) % 32))
        lo = [min(g[0][k] for g in groups) for k in range(3)]
        hi = [max(g[1][k] for g in groups) for k in range(3)]
        HEADER.pack_into(out, 0, DCL_MAGIC, DCL_VERSION, len(self.textures), len(self.materials), len(groups),
                         len(runs), len(owners), vertex_count, index_count, o_textures, o_materials, o_groups,
                         o_owners, o_runs, o_positions, o_attributes, o_indices, o_texdata,
                         len(out) - o_texdata, *lo, *hi, 0)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(out)
        return {"groups": len(groups), "runs": len(runs), "vertices": vertex_count, "indices": index_count,
                "triangles": triangle_count, "textures": len(self.textures), "texture KB": len(texdata) // 1024,
                "bytes": len(out)}
