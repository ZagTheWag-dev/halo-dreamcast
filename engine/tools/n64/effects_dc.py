"""The Dreamcast's particles (fx/<map>.dcf, port/dc/src/dc_fx.c), written by
convert.py --dc --filesystem.

The N64's records (effects.py: each effect event's emitters, the 128x64 I4
atlas of the particles' pictures and each cell's colour, the explosions the
N64 draws its own way), little-endian: dc_fx.c reads the emitters in place,
as n64_particles.c does (struct n64_fx_emitter). The atlas's texels stay
I4; dc_fx.c makes them an ARGB4444 texture at load.

And what the N64 never drew: the projectiles that have no model of their
own (plasma bolts, bullets, the tank's shell). Halo shows those by their
attachments, a light (a glowing ball: Halo's lens flare) and a contrail (a
tracer, a vapour trail), and the converter drops both (convert.py
DROPPED_GROUPS 'cont'; a light is only a light). Here each projectile tag
gets a look from them, drawn by dc_fx.c where the projectile flies
(projectiles.c, HALO_DC): a glow, from its first light's colour (the
brighter of its bounds) for a projectile with no model; a trail, from its
first contrail's first point state (colour, width; added to the frame for
Halo's add blends, else blended by alpha). The sniper rifle's bullet has
the N64's own ribbon and the flamethrower's flame its own trail: none here.

File (little-endian): effects.py's, with

    header   'DCFX', u32 version (1), then as effects.py's (entry count,
             cell count, index offset, atlas offset, explosions offset and
             count, the flame's cell, a pad byte), u32 the looks' offset
    looks    u16 count, u16 pad, LOOK * count (sorted by tag index): u16
             projectile tag index, u8 flags (LOOK_GLOW, LOOK_TRAIL,
             LOOK_ADDITIVE), pad, u8 glow RGBA, u8 trail RGBA, f32 glow
             radius, f32 trail width (world units)
"""

import struct

from .effects import EffectParticles

MAGIC = b"DCFX"
VERSION = 1
LOOK = struct.Struct("<HBx4B4Bff")
assert LOOK.size == 20
LOOK_GLOW = 1
LOOK_TRAIL = 2
LOOK_ADDITIVE = 4
# the projectiles with drawing of their own (dc_fx.c: the N64's sniper
# ribbon, the flame trail)
OWN_LOOKS = ("weapons\\sniper rifle\\", "weapons\\flamethrower\\")
# a glow's radius: the light's, scaled (a plasma pistol's light reaches 1
# unit, its bolt is a ball of about a tenth)
GLOW_SCALE = 0.08
GLOW_MINIMUM, GLOW_MAXIMUM = 0.06, 0.14


def _fields(defs, name):
    return {f.name: o for o, f in defs.fields(name)}


def _brightest(colours):
    """the brighter of ARGB float colours as RGB bytes (brightest channel
    255), or None when both are black"""
    best = max(colours, key=lambda c: c[1] + c[2] + c[3])
    top = max(best[1:])
    if top <= 0.02:
        return None
    return tuple(max(0, min(255, int(round(c * 255 / top)))) for c in best[1:])


class DcEffectParticles(EffectParticles):
    ORDER = "<"

    def __init__(self, xmap, defs):
        super().__init__(xmap, defs)
        self.of = _fields(defs, "Object")
        self.af = _fields(defs, "ObjectAttachment")
        self.lf = _fields(defs, "Light")
        self.cf = _fields(defs, "Contrail")
        self.psf = _fields(defs, "ContrailPointState")

    def _floats(self, address, n):
        return struct.unpack(f"<{n}f", self.map.read(address, 4 * n))

    def looks(self):
        """(tag index, LOOK bytes) for each projectile tag that has one"""
        out = []
        for tag in self.map.tags:
            if tag.group_name != "proj" or tag.name.startswith(OWN_LOOKS):
                continue
            model = self._dep(tag.address + self.of["model"])
            count, pointer = self._block(tag.address + self.of["attachments"])
            light = contrail = None
            for i in range(max(count, 0)):
                attached = self._dep(pointer + i * self.defs.size("ObjectAttachment") + self.af["type"])
                if attached is None:
                    continue
                if attached.group_name == "ligh" and light is None:
                    light = attached
                elif attached.group_name == "cont" and contrail is None:
                    contrail = attached
            flags = 0
            glow, trail = (0, 0, 0, 0), (0, 0, 0, 0)
            radius = width = 0.0
            if light is not None and model is None:
                colour = _brightest([self._floats(light.address + self.lf["color lower bound"], 4),
                                     self._floats(light.address + self.lf["color upper bound"], 4)])
                if colour is not None:
                    flags |= LOOK_GLOW
                    glow = colour + (255,)
                    reach = self._floats(light.address + self.lf["radius"], 1)[0]
                    radius = max(GLOW_MINIMUM, min(GLOW_MAXIMUM, reach * GLOW_SCALE))
            if contrail is not None:
                states, states_pointer = self._block(contrail.address + self.cf["point states"])
                if states > 0:
                    lower = self._floats(states_pointer + self.psf["color lower bound"], 4)
                    upper = self._floats(states_pointer + self.psf["color upper bound"], 4)
                    colour = _brightest([lower, upper])
                    if colour is not None:
                        alpha = max(lower[0], upper[0])
                        flags |= LOOK_TRAIL
                        trail = colour + (max(0, min(255, int(round(alpha * 255)))),)
                        width = self._floats(states_pointer + self.psf["width"], 1)[0]
                        blend = struct.unpack("<H", self.map.read(
                            contrail.address + self.cf["framebuffer blend function"], 2))[0]
                        if blend in (3, 7):
                            flags |= LOOK_ADDITIVE
            if flags:
                out.append((tag.index, LOOK.pack(tag.index, flags, *glow, *trail, radius, width)))
        return sorted(out)

    def pack(self):
        out, stats = super().pack()
        out[0:8] = MAGIC + struct.pack("<I", VERSION)
        looks = self.looks()
        out += b"\0" * ((-len(out)) % 4)
        struct.pack_into("<I", out, 28, len(out))
        out += struct.pack("<HH", len(looks), 0)
        for _, look in looks:
            out += look
        stats["projectile looks"] = len(looks)
        stats["bytes"] = len(out)
        return out, stats
