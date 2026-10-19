"""The Dreamcast's object models (levels/<map>.dcm, port/dc/src/dc_model.c):
the N64's model set (models.py ModelConverter: which models, their parts
in node space -- rigid, one node a vertex --, their materials, budgets, far
versions), written little-endian for the PowerVR.

A batch is one part's triangles of one material: its vertices sorted by
node (ranges: a node and how many vertices, each range transformed by its
node's matrix), its triangles as strips (level.py triangle_strips). The
textures are level_dc.py's, VQ compressed (pvrtex): square, twiddled, mipmapped, 16-bit;
the Xbox's images downscaled to at most WORLD_TEXTURE_SIZE a side for the
world's models and FIRST_PERSON_TEXTURE_SIZE for the first-person weapons
and hands (no N64 atlas windows). The meshes are Halo's most detailed
(LOD_PREFERENCE), the N64's budgets times BUDGET_SCALE. A change-colour
texture has an overlay (its index + 1 in its record's first reserved word):
the texture with the colour mask as its alpha, drawn over it tinted.

File layout (little-endian, sections 4-aligned, model data and texture
data 32-aligned):

    header        HEADER
    textures      level_dc.TEXTURE * texture_count
    models        MODEL * model_count
    regions       REGION * region_count (permutation count, first)
    permutations  i16 * permutation_count (geometry, -1: none)
    geometries    GEOMETRY * geometry_count (first batch, batch count)
    batches       BATCH * batch_count
    ranges        RANGE * range_count (node, vertex count)
    model data    per model: VERTEX * vertex_count, then u16 indices
                  (batch-local; bit 15 on each strip's last)
    texdata       the textures' PowerVR data
"""

import struct
from pathlib import Path

from . import level as L
from . import models as M
from .level import triangle_strips
from .level_dc import DcLevelBuilder, TEXTURE, TEXTURE_MIPMAPPED, argb, decode_size

DCM_MAGIC = b"DCM1"
DCM_VERSION = 1
WORLD_TEXTURE_SIZE = 128
FIRST_PERSON_TEXTURE_SIZE = 256
# Halo's own meshes, the most detailed first, simplified only past these
# budgets (triangles a model; the N64's: models.py GROUP_BUDGETS, x1)
LOD_PREFERENCE = ("super high", "high", "medium", "low", "super low")
BUDGET_SCALE = 3.0
FIRST_PERSON_BUDGET = 2000

HEADER = struct.Struct("<4sI8H10I")
MODEL = struct.Struct("<HBBHHHHHHIII")
REGION = struct.Struct("<HH")
GEOMETRY = struct.Struct("<HH")
BATCH = struct.Struct("<hHHHIHHII")
RANGE = struct.Struct("<BxH")
VERTEX = struct.Struct("<3f2fIII")

assert (HEADER.size, MODEL.size, BATCH.size, RANGE.size, VERTEX.size) == (64, 28, 24, 4, 32)

# a weapon's display (display_texture): the batch's texture the first of
# its frames, the frame picked by the object's function value (dc_model.c);
# the frames' first texture's second reserved word: DISPLAY_VALID | the
# function (0-3: A-D) << 16 | the counter's limit << 8 | the frame count
MATERIAL_DISPLAY = 0x4000
# (a numeric display's tens digit; its ones without)
MATERIAL_DISPLAY_TENS = 0x2000
DISPLAY_VALID = 0x80000000
# a model shader's self-illumination (Halo's multipurpose map's green
# channel times the shader's self-illumination colour: the plasma weapons'
# lights, the Covenant's glowing parts): the batch's texture's glow texture
# (its second reserved word: GLOW_VALID | the glow's index), added over the
# batch where the flag is set (dc_model.c)
MATERIAL_SELF_ILLUM = 0x0080  # (level.py MATERIAL_DETAIL, which models never use; 0x8000 is models.py DECAL)
GLOW_VALID = 0x40000000
# (a mask this faint or a colour this dark: no glow)
GLOW_MASK_MINIMUM = 32
GLOW_COLOUR_MINIMUM = 0.05
GLOW_FIRST_PERSON_SIZE = 128
GLOW_WORLD_SIZE = 64
METER_FRAMES = 16
# a map's sky (models_dc.py object_models): drawn about the eye at this
# radius (inside the Dreamcast's far plane, past the level), its triangles
# kept, its layers' textures at most this size; the layer drawn solid
MATERIAL_SKY = 64
SKY_RADIUS = 350.0
SKY_INNER_RADIUS = 250.0
SKY_BUDGET = 6000
SKY_TEXTURE_SIZE = 256
# (a sky's base layer: Blood Gulch's "sky clear blue", a30's "sky dusk")
SKY_SOLID = ("sky ",)
# the displays' frames' sides (texels)
DIGIT_SIZE = 64
METER_SIZE = 32


class DcModelConverter(M.ModelConverter):
    def __init__(self, xmap, defs, map_name, out_dir):
        super().__init__(xmap, defs, map_name, out_dir)
        textures = DcLevelBuilder(xmap, defs, map_name, out_dir)
        textures.texture_prefix = "m"
        # (models.py: a model's colour is not doubled, its light baked whole)
        textures.texture_brightness = 1.0
        textures.texture_limit = WORLD_TEXTURE_SIZE
        textures.vq = True
        textures.texture_display = {}
        # a texture -> its glow texture (self_illumination)
        textures.texture_glow = {}
        self.glows = {}
        self.display_frames = {}
        # the map's skies' models (convert.py: the scenario's skies), drawn
        # about the eye behind everything (dc_render.c n64_render_view)
        self.sky_models = set()
        self.sky_shells = {}
        self.textures = textures
        self.budget_scale = BUDGET_SCALE
        self.first_person_budget = FIRST_PERSON_BUDGET

    def object_models(self):
        """the objects' models, and the skies' (at SKY_RADIUS about the eye,
        their own light, no far version: models.py's ui sky)"""
        found = super().object_models()
        for tag in self.map.tags:
            if tag.index in self.sky_models and tag not in found:
                radius = self.model_radius(tag)
                if radius <= 0:
                    continue
                found.append(tag)
                self.sky_shells[tag.index] = (SKY_INNER_RADIUS, SKY_RADIUS, radius)
                self.extra_flags[tag.index] = MATERIAL_SKY | L.MATERIAL_TWO_SIDED
                self.no_far.add(tag.index)
                self.budgets[tag.index] = SKY_BUDGET
        return found

    def model_radius(self, tag):
        """the farthest of a model's vertices from its origin"""
        mf, gf = self.f("Model"), self.f("ModelGeometry")
        pf = self.f("ModelGeometryPart")
        gsize, psize = self.defs.size("ModelGeometry"), self.defs.size("ModelGeometryPart")
        gc, gp = self.block(tag.address + mf["geometries"])
        radius = 0.0
        for g in range(max(gc, 0)):
            pc, pp = self.block(gp + g * gsize + gf["parts"])
            for p in range(max(pc, 0)):
                raw = self.map.read(pp + p * psize, psize)
                count = struct.unpack_from("<I", raw, pf["vertex count"])[0]
                entry = struct.unpack_from("<I", raw, pf["vertex offset"])[0]
                if not count or not entry:
                    continue
                pointer = struct.unpack("<I", self.map.read(entry + 4, 4))[0]
                data = self.map.read(pointer, count * 32)
                for v in range(count):
                    x, y, z = struct.unpack_from("<3f", data, v * 32)
                    radius = max(radius, (x * x + y * y + z * z) ** 0.5)
        return radius

    def sky_texture(self, shader_datum):
        """a sky's layer: its first map whole, the sky's colour solid, its
        clouds, planets and ring blended by their alpha (Halo's sky
        shaders are transparent ones: as glows they were faint)"""
        tag = self.map.tag(shader_datum)
        if tag is None:
            return None, 0, None
        images = self.first_bitmap_images(tag)
        if not images:
            return None, 0, None
        try:
            img = M.decode(self.map, images[0], SKY_TEXTURE_SIZE).convert("RGBA")
        except ValueError:
            return None, 0, None
        solid = img.getchannel("A").getextrema()[0] >= 250 or tag.name.rsplit("\\", 1)[-1].startswith(SKY_SOLID)
        if solid:
            return self.textures.write_texture(img.convert("RGB").convert("RGBA"), tag.name,
                                               limit=SKY_TEXTURE_SIZE), 0, None
        return self.textures.write_texture(img, tag.name, fmt="ARGB4444", limit=SKY_TEXTURE_SIZE),             L.MATERIAL_TRANSPARENT | L.MATERIAL_BLEND, None

    def display_texture(self, shader_datum, permutation):
        """a weapon's display that Halo draws from the weapon's state, as a
        run of frames (consecutive textures) dc_model.c picks from by one of
        the object's function values: a numeric shader's digits (the
        assault rifle's ammunition counter, its compass) or a meter's fill
        (the plasma weapons' heat gauges), baked at METER_FRAMES levels.
        (texture, flags, None), or None if the shader is neither"""
        tag = self.map.tag(shader_datum)
        if tag is None or tag.group_name not in ("schi", "smet"):
            return None
        key = ("display", shader_datum, max(0, permutation))
        if key in self.masked:
            return self.masked[key]
        result = None
        if tag.group_name == "schi":
            cf = self.f("ShaderTransparentChicago")
            flags = self.map.read(tag.address + cf["shader transparent chicago flags"], 1)[0]
            if flags & 0x80:
                result = self.numeric_frames(tag, cf, permutation, flags)
        else:
            result = self.meter_frames(tag)
        self.masked[key] = result
        return result

    def first_bitmap_images(self, shader_tag):
        found = []

        def on_dependency(t, address, f, path, group, datum):
            if group == 0x6269746D and datum != 0xFFFFFFFF and not found:  # 'bitm'
                found.append(datum)

        from .walk import Walker
        Walker(self.map, self.defs).walk(shader_tag, on_dependency=on_dependency)
        bitmap = self.map.tag(found[0]) if found else None
        return M.bitmap_images(self.map, bitmap, self.defs) if bitmap is not None else []

    def write_frames(self, frames, name, limit):
        """the frames as consecutive textures (at most limit texels a side:
        small on the screen), shared by every reference to the shader"""
        if name in self.display_frames:
            return self.display_frames[name]
        first = len(self.textures.textures)
        for img in frames:
            self.textures.write_texture(img, name, limit=limit)
        first = first if len(self.textures.textures) == first + len(frames) else -1
        self.display_frames[name] = first
        return first

    def numeric_frames(self, tag, cf, permutation, shader_flags):
        """a numeric shader: its first map's images, one a digit
        (rasterizer_xbox_transparent_geometry.c: the counter, round(limit x
        function A -- D for an eight-image map, the compass), in the
        permutation's place)"""
        images = self.first_bitmap_images(tag)
        if not 2 <= len(images) <= 16 or permutation not in (0, 1):
            return None
        frames = []
        for image in images:
            try:
                frames.append(M.decode(self.map, image, 64).convert("RGBA"))
            except ValueError:
                return None
        first = self.write_frames(frames, tag.name, DIGIT_SIZE)
        if first < 0:
            return None
        limit = self.map.read(tag.address + cf["numeric counter limit"], 1)[0] or 99
        function = 3 if len(frames) == 8 else 0
        self.textures.texture_display[first] = DISPLAY_VALID | function << 16 | limit << 8 | len(frames)
        flags = L.MATERIAL_TRANSPARENT | L.MATERIAL_GLOW | MATERIAL_DISPLAY
        if shader_flags & 4:
            flags |= L.MATERIAL_TWO_SIDED
        if permutation == 1:
            flags |= MATERIAL_DISPLAY_TENS
        return first, flags, None

    def meter_frames(self, tag):
        """a meter shader (shader_transparent_meter): its map's alpha the
        gradient, filled to the value (the value source's function) in the
        gradient's colours, the rest its background, baked at METER_FRAMES
        values"""
        from PIL import Image
        images = self.first_bitmap_images(tag)
        if not images:
            return None
        try:
            img = M.decode(self.map, images[0], 64).convert("RGBA")
        except ValueError:
            return None
        mf = self.f("ShaderTransparentMeter")
        a = tag.address
        lo = struct.unpack("<3f", self.map.read(a + mf["gradient min color"], 12))
        hi = struct.unpack("<3f", self.map.read(a + mf["gradient max color"], 12))
        back = struct.unpack("<3f", self.map.read(a + mf["background color"], 12))
        tint = struct.unpack("<3f", self.map.read(a + mf["meter tint color"], 12))
        meter_alpha, back_alpha = struct.unpack("<2f", self.map.read(a + mf["meter transparency"], 8))
        source = struct.unpack("<H", self.map.read(a + mf["value source"], 2))[0]
        if not 1 <= source <= 4:
            return None
        tint = tuple(t if any(tint) else 1.0 for t in tint)
        # (transparencies: 0 opaque)
        meter_opacity, back_opacity = 1.0 - meter_alpha, 1.0 - back_alpha
        px = img.load()
        frames = []
        for k in range(METER_FRAMES):
            value = k / (METER_FRAMES - 1)
            # the meter's colour from the gradient's ends by its value (the
            # plasma weapons': green cool, red hot)
            colour = [(l + (h - l) * value) * t * meter_opacity for l, h, t in zip(lo, hi, tint)]
            background = [v * back_opacity for v in back]
            out = Image.new("RGB", img.size)
            op = out.load()
            for y in range(img.height):
                for x in range(img.width):
                    r, g, b, shape = px[x, y]
                    # the colour the fill order, the alpha the shape
                    order = (r + g + b) / 765.0
                    c = colour if value > 0 and order <= value else background
                    op[x, y] = tuple(max(0, min(255, int(v * shape))) for v in c)
            frames.append(out.convert("RGBA"))
        first = self.write_frames(frames, tag.name, METER_SIZE)
        if first < 0:
            return None
        self.textures.texture_display[first] = DISPLAY_VALID | (source - 1) << 16 | (METER_FRAMES - 1) << 8 | \
            METER_FRAMES
        return first, L.MATERIAL_TRANSPARENT | L.MATERIAL_GLOW | MATERIAL_DISPLAY, None

    def shader_texture(self, shader_datum):
        texture, flags, source = super().shader_texture(shader_datum)
        if texture is not None and texture >= 0 and not self.current_flags & MATERIAL_SKY:
            glow = self.self_illumination(shader_datum)
            if glow is not None:
                self.textures.texture_glow.setdefault(texture, glow)
                if self.textures.texture_glow[texture] == glow:
                    flags |= MATERIAL_SELF_ILLUM
        return texture, flags, source

    def self_illumination(self, shader_datum):
        """a model shader's glow texture (MATERIAL_SELF_ILLUM): its base map
        times its multipurpose map's green (self-illumination) times its
        self-illumination colour (the animation's upper bound: Halo's at
        full), or None"""
        from PIL import Image, ImageChops

        if shader_datum in self.glows:
            return self.glows[shader_datum]
        self.glows[shader_datum] = None
        tag = self.map.tag(shader_datum)
        if tag is None or tag.group_name != "soso":
            return None
        so = self.f("ShaderModel")
        read = lambda field, fmt: struct.unpack(fmt, self.map.read(tag.address + so[field], struct.calcsize(fmt)))
        colour = read("animation color upper bound", "<3f")
        base = self.map.tag(read("base map", "<12xI")[0])
        multipurpose = self.map.tag(read("multipurpose map", "<12xI")[0])
        if max(colour) < GLOW_COLOUR_MINIMUM or base is None or multipurpose is None:
            return None
        try:
            img = L.decode(self.map, L.bitmap_images(self.map, base, self.defs)[0], self.textures.texture_limit)
            mask = L.decode(self.map, L.bitmap_images(self.map, multipurpose, self.defs)[0], max(img.size))
        except (ValueError, IndexError):
            return None
        mask = mask.convert("RGBA").getchannel("G").resize(img.size, Image.BILINEAR)
        if mask.getextrema()[1] < GLOW_MASK_MINIMUM:
            return None
        bands = [band.point(lambda v, k=min(1.0, c): int(v * k)) for band, c in zip(img.convert("RGB").split(), colour)]
        glow = Image.merge("RGB", [ImageChops.multiply(band, mask) for band in bands]).convert("RGBA")
        # (small: a glow is soft, and video memory short -- a30's had 84 KB
        # left with them at the base map's size)
        limit = GLOW_FIRST_PERSON_SIZE if self.textures.texture_limit == FIRST_PERSON_TEXTURE_SIZE else GLOW_WORLD_SIZE
        self.glows[shader_datum] = self.textures.write_texture(glow, base.name + " glow", limit=limit)
        return self.glows[shader_datum]

    def build_batches(self, vertices, triangles, texture, flags, st_of, batches, vertex_data, index_data):
        """one batch: the triangles' vertices sorted by node (models.py's
        vertices: node, position in node space, uv, shade or colour,
        normal), their strips; vertex_data and index_data are the model's"""
        if not triangles:
            return
        used = sorted({i for t in triangles for i in t}, key=lambda i: (vertices[i][0], i))
        local = {v: k for k, v in enumerate(used)}
        first_vertex = len(vertex_data) // VERTEX.size
        first_index = len(index_data) // 2
        ranges = []
        for i in used:
            v = vertices[i]
            node = v[0]
            if ranges and ranges[-1][0] == node:
                ranges[-1][1] += 1
            else:
                ranges.append([node, 1])
            u, w = st_of(v[2]) if st_of is not None else v[2]
            if isinstance(v[3], tuple):
                colour = argb(v[3])
            else:
                colour = argb((v[3], v[3], v[3]))
            vertex_data += VERTEX.pack(*v[1], u, w, colour, 0, 0)
        for strip in triangle_strips([tuple(local[i] for i in t) for t in triangles]):
            index_data += struct.pack(f"<{len(strip)}H", *strip[:-1], strip[-1] | 0x8000)
        batches.append((texture, flags & ~M.DECAL, ranges, len(triangles), first_vertex, len(used), first_index,
                        len(index_data) // 2 - first_index))

    def write(self, path):
        """models.py's walk of the models (regions, permutations,
        geometries, far versions), written for the Dreamcast"""
        saved = M.DETAIL_WINDOWS, M.LOD_PREFERENCE
        M.DETAIL_WINDOWS, M.LOD_PREFERENCE = False, LOD_PREFERENCE
        try:
            with decode_size(FIRST_PERSON_TEXTURE_SIZE, M):
                return self._write(path)
        finally:
            M.DETAIL_WINDOWS, M.LOD_PREFERENCE = saved

    def _write(self, path):
        mf, nf, rf, pf, gf = (self.f(s) for s in ("Model", "ModelNode", "ModelRegion", "ModelRegionPermutation",
                                                    "ModelGeometry"))
        nsize, rsize, psize, gsize = (self.defs.size(s) for s in ("ModelNode", "ModelRegion",
                                                                   "ModelRegionPermutation", "ModelGeometry"))
        model_records, region_records, permutation_records, geometry_records = [], [], [], []
        batches = []
        model_data = []
        for tag in self.object_models():
            vertex_data, index_data = bytearray(), bytearray()
            first_batch = len(batches)
            self.current_scale = self.position_scales.get(tag.index, 1.0)
            self.current_shell = self.sky_shells.get(tag.index)
            self.current_flags = self.extra_flags.get(tag.index, 0)
            first_person = tag.index in self.first_person
            self.textures.texture_limit = FIRST_PERSON_TEXTURE_SIZE if first_person else WORLD_TEXTURE_SIZE
            a = tag.address
            u_scale, v_scale = struct.unpack("<2f", self.map.read(a + mf["base map u scale"], 8))
            u_scale = u_scale or 1.0
            v_scale = v_scale or 1.0
            nc, np_ = self.block(a + mf["nodes"])
            nodes_inverse = []
            for i in range(nc):
                raw = self.map.read(np_ + i * nsize, nsize)
                scale = struct.unpack_from("<f", raw, nf["scale"])[0]
                rot = struct.unpack_from("<9f", raw, nf["rotation"])
                trans = struct.unpack_from("<3f", raw, nf["translation"])
                nodes_inverse.append(M.matrix4x3((scale,) + rot + trans))
            sc, sp = self.block(a + mf["shaders"])
            shaders = []
            for i in range(sc):
                datum = struct.unpack("<I", self.map.read(sp + i * 32 + 12, 4))[0]
                # (the reference's permutation: a numeric shader's digit)
                permutation = struct.unpack("<h", self.map.read(sp + i * 32 + 16, 2))[0]
                if tag.index in self.sky_models:
                    shaders.append(self.sky_texture(datum))
                else:
                    shaders.append(self.display_texture(datum, permutation) or self.shader_texture(datum))
            gc, gp = self.block(a + mf["geometries"])
            geometry_map = {}
            budget = self.budgets.get(tag.index, M.MODEL_BUDGET)

            def geometry(gi):
                if gi in geometry_map:
                    return geometry_map[gi]
                first = len(batches)
                pc, pp = self.block(gp + gi * gsize + gf["parts"])
                part_size = self.defs.size("ModelGeometryPart")
                total = sum(self.part_triangle_count(pp + p * part_size) for p in range(pc))
                keep = min(1.0, budget / total) if total else 1.0
                for p in range(pc):
                    self.convert_part(pp + p * part_size, nodes_inverse, u_scale, v_scale, shaders, batches,
                                      vertex_data, index_data, keep, detail=first_person, hands=tag.index in self.hands)
                geometry_map[gi] = len(geometry_records)
                geometry_records.append((first, len(batches) - first))
                return geometry_map[gi]

            rc, rp = self.block(a + mf["regions"])
            first_region = len(region_records)
            for r in range(rc):
                pc, pp = self.block(rp + r * rsize + rf["permutations"])
                first_permutation = len(permutation_records)
                for p in range(max(pc, 0)):
                    raw = self.map.read(pp + p * psize, psize)
                    choice = -1
                    for lod in self.lod_preferences.get(tag.index, M.LOD_PREFERENCE):
                        gi = struct.unpack_from("<h", raw, pf[lod])[0]
                        if 0 <= gi < gc:
                            choice = geometry(gi)
                            break
                    permutation_records.append(choice)
                region_records.append((max(pc, 0), first_permutation))
            near_count = len(batches) - first_batch
            far_first = len(batches)
            if not first_person and tag.index not in self.units and tag.index not in self.no_far:
                marks = len(vertex_data), len(index_data)
                self.convert_far(a, gc, gp, nodes_inverse, u_scale, v_scale, shaders, budget, batches, vertex_data,
                                 index_data)
                first_geometries = {permutation_records[region_records[r][1]] for r in range(first_region,
                                                                                            len(region_records))
                                    if region_records[r][0] > 0}
                near_triangles = sum(batches[b][3] for g in first_geometries if g >= 0
                                     for b in range(geometry_records[g][0], geometry_records[g][0] +
                                                    geometry_records[g][1]))
                far_triangles = sum(b[3] for b in batches[far_first:])
                if any(b[1] & L.MATERIAL_ALPHA_TEST for b in batches[far_first:]) and \
                        far_triangles > M.FAR_CUTOUT_SHARE * near_triangles:
                    del batches[far_first:]
                    del vertex_data[marks[0]:]
                    del index_data[marks[1]:]
            model_records.append((tag.index, nc, rc, first_region, first_batch, near_count,
                                  far_first - first_batch, len(batches) - far_first, first_person))
            model_data.append((vertex_data, index_data))
        return self.serialise(path, model_records, region_records, permutation_records, geometry_records, batches,
                              model_data)

    def serialise(self, path, model_records, region_records, permutation_records, geometry_records, batches,
                  model_data):
        ranges = []
        batch_table = bytearray()
        for texture, flags, batch_ranges, triangle_count, first_vertex, vertex_count, first_index, index_count in batches:
            batch_table += BATCH.pack(texture, flags, len(ranges), len(batch_ranges), first_vertex, vertex_count,
                                      min(triangle_count, 0xFFFF), first_index, index_count)
            ranges.extend(batch_ranges)

        out = bytearray(HEADER.size)

        def section(data, align=4):
            out.extend(b"\0" * (-len(out) % align))
            offset = len(out)
            out.extend(data)
            return offset

        textures = self.textures
        texdata = bytearray()
        texture_table = bytearray()
        for i, (t, (data, texture_format)) in enumerate(zip(textures.textures, textures.texture_data)):
            texdata.extend(b"\0" * (-len(texdata) % 32))
            overlay = textures.texture_overlay.get(i, -1) + 1
            texture_table += TEXTURE.pack(len(texdata), len(data), t.width, t.height, texture_format,
                                          TEXTURE_MIPMAPPED, argb(t.colour), overlay,
                                          textures.texture_display.get(i, 0) or
                                          (GLOW_VALID | textures.texture_glow[i] if i in textures.texture_glow else 0))
            texdata += data
        o_textures = section(texture_table)
        o_models = len(out)
        out.extend(b"\0" * (MODEL.size * len(model_records)))
        o_regions = section(b"".join(REGION.pack(*r) for r in region_records))
        o_permutations = section(struct.pack(f"<{len(permutation_records)}h", *permutation_records))
        o_geometries = section(b"".join(GEOMETRY.pack(*g) for g in geometry_records))
        o_batches = section(batch_table)
        o_ranges = section(b"".join(RANGE.pack(min(node, 255), count) for node, count in ranges))
        data_offsets = []
        for vertex_data, index_data in model_data:
            data_offsets.append(section(bytes(vertex_data) + bytes(index_data), 32))
        o_data = data_offsets[0] if data_offsets else len(out)
        o_texdata = section(texdata, 32)
        out.extend(b"\0" * (-len(out) % 32))
        for i, ((tag_index, nc, rc, first_region, first_batch, near_count, far_offset, far_count, first_person),
                (vertex_data, index_data)) in enumerate(zip(model_records, model_data)):
            MODEL.pack_into(out, o_models + i * MODEL.size, tag_index, min(nc, 255), rc, first_region,
                            1 if first_person else 0, first_batch, near_count, far_offset, far_count,
                            data_offsets[i], len(vertex_data) // VERTEX.size, len(index_data) // 2)
        HEADER.pack_into(out, 0, DCM_MAGIC, DCM_VERSION, len(textures.textures), len(model_records),
                         len(region_records), len(permutation_records), len(geometry_records), len(batches),
                         len(ranges), 0, o_textures, o_models, o_regions, o_permutations, o_geometries, o_batches,
                         o_ranges, o_data, o_texdata, len(out) - o_texdata)
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_bytes(out)
        return {"models": len(model_records), "batches": len(batches), "ranges": len(ranges),
                "vertices": sum(len(v) for v, _ in model_data) // VERTEX.size,
                "triangles": sum(b[3] for b in batches), "textures": len(textures.textures),
                "texture KB": len(texdata) // 1024, "bytes": len(out)}
