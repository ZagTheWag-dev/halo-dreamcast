"""The scene behind Halo's main menu for the Dreamcast (port/dc/src/dc_menu.c):
ui.map's space sky and the ring (ui.py UIModels: the ring's inner surface
lit by its shadow, the planets and the shadow's own part left out, the sky
small about the eye) as models_dc.py's models, levels/ui.dcm, and the
scene, levels/ui.dcs: the sky's and the ring's model tag indices, the
ring's node matrix and the ui scenario's camera points, little-endian.

The Dreamcast takes the textures whole: no N64 windows or cells; the stars
baked at SKY_SIZE with SKY_STARS of them, the ring's inner landscapes at
RING_SIZE, its outer plating at its own colours (the N64's grey-and-tint
trick left out).

    python -m tools.n64.ui_dc <ui.map> --filesystem <file tree>
"""

import argparse
import struct
from pathlib import Path

from . import models as M
from . import ui as U
from .models_dc import DcModelConverter
from .repack import Repacker
from .xboxmap import XboxMap

SKY_SIZE = 512
SKY_STARS = 5000
RING_SIZE = 512
# a model batch drawn first, without depth (ui.py MATERIAL_SKY: dc_model.c)
MATERIAL_SKY = U.MATERIAL_SKY

SCENE_MAGIC = b"DCS1"
# magic, the sky's and the ring's model tag indices, the ring's matrix (13
# floats), the camera points' count; then each: name[32], position,
# forward, up, horizontal field of view (10 floats)
SCENE_HEADER = struct.Struct("<4sHH13fI")
SCENE_POINT = struct.Struct("<32s10f")


class DcUIModels(U.UIModels, DcModelConverter):
    """ui.py's scene models through models_dc.py"""

    def shader_texture(self, shader_datum):
        tag = self.map.tag(shader_datum)
        if tag is not None and tag.name.endswith("\\space"):
            image = U.space_backdrop(self.map, self.defs, tag, size=SKY_SIZE, stars=SKY_STARS)
            return self.textures.write_texture(image, tag.name, limit=SKY_SIZE), 0, None
        if tag is not None and tag.name.endswith(U.SKY_LEFT_OUT + (U.RING_SHADOW,)):
            return None, 0, None
        if tag is not None and tag.name.endswith(U.RING_INNER):
            limit, self.textures.texture_limit = self.textures.texture_limit, RING_SIZE
            try:
                texture, flags, _ = M.ModelConverter.shader_texture(self, shader_datum)
            finally:
                self.textures.texture_limit = limit
            return texture, flags, ("ring",)
        return M.ModelConverter.shader_texture(self, shader_datum)

    def texture_windows(self, bitmap, vertices, triangles, cutout=False):
        # the ring's landscapes self-lit under its shadow (ui.py), the
        # texture whole
        if bitmap is not None and bitmap[0] == "ring":
            shadow = self.ring_shadow()
            for i in {i for tri in triangles for i in tri}:
                v = vertices[i]
                vertices[i] = (v[0], v[1], v[2], shadow(v[1]), v[4])
        return None

    def convert_part(self, part_address, nodes_inverse, u_scale, v_scale, *rest, **options):
        # (texture_windows called for these models: it lights the ring)
        saved = M.DETAIL_WINDOWS
        M.DETAIL_WINDOWS = True
        try:
            return U.UIModels.convert_part(self, part_address, nodes_inverse, u_scale, v_scale, *rest, **options)
        finally:
            M.DETAIL_WINDOWS = saved


def write_scene(xmap, defs, path):
    scenario = xmap.scenario
    f = {fl.name: o for o, fl in defs.fields("Scenario")}
    a = scenario.address
    sc, sp = struct.unpack("<iI", xmap.read(a + f["scenery"], 8))
    sf = {fl.name: o for o, fl in defs.fields("ScenarioScenery")}
    ring = (1.0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
    if sc > 0:
        raw = xmap.read(sp, defs.size("ScenarioScenery"))
        position = struct.unpack_from("<3f", raw, sf["position"])
        forward, left, up = U.rotation(*struct.unpack_from("<3f", raw, sf["rotation"]))
        ring = (1.0,) + forward + left + up + position
    cc, cp = struct.unpack("<iI", xmap.read(a + f["cutscene camera points"], 8))
    cf = {fl.name: o for o, fl in defs.fields("ScenarioCutsceneCameraPoint")}
    csize = defs.size("ScenarioCutsceneCameraPoint")
    sky = [t.index for t in xmap.tags if t.group_name == "mode" and t.name == U.SKY]
    ring_model = [t.index for t in xmap.tags if t.group_name == "mode" and t.name == U.RING]
    points = bytearray()
    for i in range(max(cc, 0)):
        raw = xmap.read(cp + i * csize, csize)
        name = raw[cf["name"]:cf["name"] + 32].split(b"\0")[0]
        position = struct.unpack_from("<3f", raw, cf["position"])
        forward, left, up = U.rotation(*struct.unpack_from("<3f", raw, cf["orientation"]))
        fov = struct.unpack_from("<f", raw, cf["field of view"])[0]
        points += SCENE_POINT.pack(name, *position, *forward, *up, fov)
    data = SCENE_HEADER.pack(SCENE_MAGIC, sky[0] if sky else 0xFFFF, ring_model[0] if ring_model else 0xFFFF,
                             *ring, max(cc, 0)) + points
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return max(cc, 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("map", type=Path)
    parser.add_argument("--filesystem", type=Path, required=True)
    args = parser.parse_args()
    xmap = XboxMap(args.map)
    defs = Repacker(xmap, None).defs
    cameras = write_scene(xmap, defs, args.filesystem / "levels" / "ui.dcs")
    models = DcUIModels(xmap, defs, "ui", args.filesystem)
    sky = [t for t in xmap.tags if t.group_name == "mode" and t.name == U.SKY]
    if sky:
        models.position_scales = {sky[0].index: U.SKY_RADIUS / 27000.0}
        models.extra_flags = {sky[0].index: MATERIAL_SKY | M.MATERIAL_TWO_SIDED}
    models.no_far = {t.index for t in xmap.tags if t.group_name == "mode"}
    stats = models.write(args.filesystem / "levels" / "ui.dcm")
    print(f"ui scene: {cameras} camera points, models " + ", ".join(f"{k} {v}" for k, v in stats.items()))


if __name__ == "__main__":
    main()
