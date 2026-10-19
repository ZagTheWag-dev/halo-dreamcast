"""The Dreamcast's sounds (snd/<map>.dcs, port/dc/src/dc_sound.c), written
by convert.py --dc --filesystem.

The records the game reads are sounds.py's, in repack.ORDER ("<" here), so
port/n64/game/n64_sound_game.c plays them as it does on the N64. The files
they play are not audioconv64's VADPCM: each permutation the game can
choose is decoded from Xbox ADPCM (sounds.py), made mono (the two channels'
mean) and compressed to the AICA's own 4-bit Yamaha ADPCM at 22050 Hz, two
samples a byte, low nibble first, from the decoder's starting state (step
127, signal 0), which the AICA also starts each play and each loop from.

An AICA channel plays at most 65535 samples (its loop end is a 16-bit
sample number), three seconds at 22050 Hz: a longer file (28 on Blood Gulch,
the longest 5.3 s) is resampled to a lower rate that fits (its play rate is
its own, dc_sound.c sets the channel's), and one longer than twice that is
cut, its last 50 ms faded out.

Blood Gulch's 858 files are 775 s, 8.5 MB of ADPCM; sound RAM is 2 MB, the
driver's 192 KB of it taken. So dc_sound.c keeps a cache: at the map's load
it fills sound RAM with the files at the bank's start (the order below),
and loads the rest from the disc as they are asked for, the least recently
played making room. Order: each pitch range's first permutation before the
other permutations, then by class (CLASS_ORDER: the announcer, weapons,
detonations, impacts, vehicles, footsteps...), then as met. Each file's
alternate is the next permutation of the pitch range it was first met in
(a ring; itself when alone): a file not in sound RAM yet plays as a loaded
one of its ring meanwhile -- the same sound, another of the takes Halo
chooses among at random anyway.

File (little-endian):

    header  'DCSB', u32 version (1), u16 file count, u16 files of rank 0,
            u32 table offset, u32 data offset, u32 data size, u32 0, u32 0
    file    (16 bytes each, in the map's list order) u32 offset (from the
            file's start, a multiple of 2048: a sector of the disc), u32
            samples, u16 rate, u16 alternate (a file number), u8 class,
            u8 rank (0: a pitch range's first permutation), u16 0
    data    each file's ADPCM, ceil(samples / 2) bytes padded to 32 (the
            AICA's DMA), from its 2048-aligned offset

The converted files are cached by content: build/dc/sound-cache/<hash>.dca
('<IH' samples, rate, then the ADPCM bytes).
"""

import math
import struct
from array import array
from pathlib import Path

from .sounds import SoundConverter, decode_xbox_adpcm

MAGIC = b"DCSB"
VERSION = 1
HEADER = struct.Struct("<4sIHHIIIII")
FILE = struct.Struct("<IIHHBBH")
assert HEADER.size == 32 and FILE.size == 16
RATE = 22050
# an AICA channel's longest sample (KallistiOS's snd_sfx keeps one short of
# the 16-bit loop end's 65535)
MAXIMUM_SAMPLES = 65534
# a file is resampled no lower than this; past it, cut
MINIMUM_RATE = RATE // 2
SECTOR = 2048
FADE_SECONDS = 0.05
# the preload order's classes (sounds.py KEPT_CLASSES): game events (the
# announcer), weapon fire, detonations, projectile impacts, the weapons'
# ready/reload/empty/charge/overheat/idle, vehicles' engines and
# collisions, footsteps, object/particle impacts, dialogue, the rest
CLASS_ORDER = (50, 4, 1, 0, 5, 6, 7, 8, 9, 10, 23, 22, 18, 13, 14, 15, 19, 39, 44, 45, 46, 47)

# the AICA's ADPCM (KallistiOS utils/wav2adpcm, superctr's ymz_codec): a
# nibble's magnitude scales the step by these /256
STEP_SCALE = (230, 230, 230, 230, 307, 409, 512, 614)


def aica_adpcm(samples):
    """16-bit samples to the AICA's 4-bit ADPCM, two a byte, low nibble
    first; each nibble the nearest the decoder can make of the next sample,
    the decoder's state followed exactly (no drift)"""
    out = bytearray((len(samples) + 1) // 2)
    signal, step = 0, 127
    scale = STEP_SCALE
    for i, sample in enumerate(samples):
        delta = sample - signal
        if delta < 0:
            sign = 8
            delta = -delta
        else:
            sign = 0
        code = (delta << 2) // step
        if code > 7:
            code = 7
        diff = ((1 + (code << 1)) * step) >> 3
        if diff > 32767:
            diff = 32767
        if sign:
            signal -= diff
            if signal < -32768:
                signal = -32768
        else:
            signal += diff
            if signal > 32767:
                signal = 32767
        step = (scale[code] * step) >> 8
        if step < 127:
            step = 127
        elif step > 24576:
            step = 24576
        if i & 1:
            out[i >> 1] |= (code | sign) << 4
        else:
            out[i >> 1] = code | sign
    return bytes(out)


def aica_adpcm_decode(data, count):
    """the AICA's decoding of aica_adpcm's bytes (checking, measuring)"""
    out = array("h")
    signal, step = 0, 127
    for i in range(count):
        nibble = (data[i >> 1] >> 4) if i & 1 else data[i >> 1] & 15
        code = nibble & 7
        diff = min(32767, ((1 + (code << 1)) * step) >> 3)
        signal = max(-32768, signal - diff) if nibble & 8 else min(32767, signal + diff)
        step = min(24576, max(127, (STEP_SCALE[code] * step) >> 8))
        out.append(signal)
    return out


def resample(samples, rate, to_rate):
    """windowed-sinc (Lanczos, 6 lobes at the lower rate's cutoff)"""
    ratio = to_rate / rate
    count = int(len(samples) * ratio)
    cutoff = min(1.0, ratio) * 0.95
    lobes = 6
    half = int(math.ceil(lobes / cutoff))
    out = array("h", bytes(2 * count))
    last = len(samples) - 1
    for n in range(count):
        t = n / ratio
        middle = int(t)
        total = weights = 0.0
        for k in range(middle - half + 1, middle + half + 1):
            x = (t - k) * cutoff
            if x == 0.0:
                w = 1.0
            elif abs(x) >= lobes:
                continue
            else:
                px = math.pi * x
                w = math.sin(px) / px * math.sin(px / lobes) / (px / lobes)
            total += w * samples[min(max(k, 0), last)]
            weights += w
        value = int(round(total / weights)) if weights else 0
        out[n] = max(-32768, min(32767, value))
    return out


def dc_samples(data, channels, rate, fmt):
    """a permutation's samples as the Dreamcast plays them: (ADPCM bytes,
    sample count, rate)"""
    pcm = decode_xbox_adpcm(data, channels) if fmt == 1 else data[:len(data) // 2 * 2]
    wide = array("h")
    wide.frombytes(pcm)
    if channels == 2:
        wide = array("h", [(wide[i] + wide[i + 1]) >> 1 for i in range(0, len(wide) - 1, 2)])
    if rate != RATE:
        wide = resample(wide, rate, RATE)
        rate = RATE
    if len(wide) > MAXIMUM_SAMPLES:
        to_rate = max(MINIMUM_RATE, int(MAXIMUM_SAMPLES * rate / len(wide)))
        wide = resample(wide, rate, to_rate)
        rate = to_rate
        if len(wide) > MAXIMUM_SAMPLES:
            wide = wide[:MAXIMUM_SAMPLES]
            fade = int(rate * FADE_SECONDS)
            for i in range(fade):
                at = MAXIMUM_SAMPLES - fade + i
                wide[at] = int(wide[at] * (fade - i) / fade)
    if not wide:
        wide = array("h", [0, 0])
    return aica_adpcm(wide), len(wide), rate


class DcSoundConverter(SoundConverter):
    """sounds.py's records, and the files as the AICA's ADPCM in one bank"""

    CACHE_VERSION = b"dc sound 1"
    SUFFIX = ".dca"

    def write(self, bank_path):
        """converts the files the cache lacks; writes snd/<map>.dcs"""
        self.cache.mkdir(parents=True, exist_ok=True)
        for key, (data, channels, rate, _, fmt) in self.pending.items():
            adpcm, samples, rate = dc_samples(data, channels, rate, fmt)
            made = self.cache / f".{key}.dca"
            made.write_bytes(struct.pack("<IH", samples, rate) + adpcm)
            made.replace(self.cache / f"{key}.dca")
        count = len(self.files)
        # each file's class, rank and alternate: from the pitch range it is
        # first met in (a file shared by several takes the best rank)
        info = [None] * count
        alternate = list(range(count))
        for sound_class, files in self.ranges:
            files = [f for f in dict.fromkeys(files) if f != 0xFFFF]
            for i, f in enumerate(files):
                rank = 0 if i == 0 else 1
                if info[f] is None:
                    info[f] = [sound_class, rank]
                    alternate[f] = files[(i + 1) % len(files)]
                elif rank < info[f][1]:
                    info[f][1] = rank
        info = [i or [0, 1] for i in info]

        def order(f):
            sound_class, rank = info[f]
            place = CLASS_ORDER.index(sound_class) if sound_class in CLASS_ORDER else len(CLASS_ORDER)
            return rank, place, f

        table_offset = HEADER.size
        data_offset = table_offset + FILE.size * count
        data_offset += (-data_offset) % SECTOR
        data = bytearray()
        entries = [None] * count
        for f in sorted(range(count), key=order):
            raw = (self.cache / f"{self.files[f]}.dca").read_bytes()
            samples, rate = struct.unpack_from("<IH", raw)
            adpcm = raw[6:]
            adpcm += b"\0" * ((-len(adpcm)) % 32)
            data += b"\0" * ((-len(data)) % SECTOR)
            entries[f] = FILE.pack(data_offset + len(data), samples, rate, alternate[f], info[f][0], info[f][1], 0)
            data += adpcm
        rank0 = sum(1 for i in info if i[1] == 0)
        out = HEADER.pack(MAGIC, VERSION, count, rank0, table_offset, data_offset, len(data), 0, 0)
        out += b"".join(entries)
        out += b"\0" * (data_offset - len(out))
        out += data
        Path(bank_path).parent.mkdir(parents=True, exist_ok=True)
        made = Path(bank_path).with_name("." + Path(bank_path).name)
        made.write_bytes(out)
        made.replace(bank_path)
        return {"files": count, "new": len(self.pending), "first takes": rank0,
                "seconds": round(self.seconds), "MB": round(len(out) / 1e6, 2)}
