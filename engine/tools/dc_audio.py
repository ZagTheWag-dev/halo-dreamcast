#!/usr/bin/env python3
"""Measures a Flycast sound capture (tools/dc_flycast_test.ps1 -audio): SDL's
"disk" driver's raw output, 16-bit little-endian stereo at 44100 Hz.

    python tools/dc_audio.py <capture.raw> [--wav out.wav] [--png out.png]
        [--compare <bank.dcs> <file>@<seconds> ...]

Prints each second's peak and RMS level, left and right (dBFS), how much of
it is clipped, and the DC offset; --wav writes the capture as a WAV file to
listen to; --png draws its waveform (each column a slice's peak, left above
right) to look at.

--compare (a capture of the "dc_sound_test" setting, dc_sound.c): each
bank file as the converter decodes it (sounds_dc.py), at the AICA's rate
for it, against the capture's left channel where it should start -- the
given seconds after the first sound's onset (the test plays file 0 first,
then one every 2 s) -- the best lag within 45 ms either side: the
correlation, the gain and how far apart they are (dB).
"""

import argparse
import math
import wave
from array import array
from pathlib import Path

RATE = 44100


def db(value):
    return 20 * math.log10(value / 32768) if value > 0 else -120.0


def aica_rate(frequency):
    """the rate the AICA plays a frequency at (its 4-bit octave and 10-bit
    mantissa, KallistiOS's aica.c aica_freq)"""
    base, octave = 5644800, 7
    while frequency < base and octave > -8:
        base >>= 1
        octave -= 1
    return 44100 * 2 ** octave * (1 + (((frequency << 10) // base) & 1023) / 1024)


def compare(left, rate, bank_path, wanted):
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from tools.n64.sounds_dc import FILE, HEADER, aica_adpcm_decode
    bank = bank_path.read_bytes()
    header = HEADER.unpack_from(bank)
    files = [FILE.unpack_from(bank, header[4] + FILE.size * i) for i in range(header[2])]
    first = next(i for i, x in enumerate(left) if abs(x) > 30)
    print(f"first sound at {first / rate:.3f} s")
    for item in wanted:
        number, seconds = item.split("@")
        offset, count, file_rate = files[int(number)][:3]
        decoded = aica_adpcm_decode(bank[offset:offset + (count + 1) // 2], count)
        step = aica_rate(file_rate) / rate
        length = min(int(count / step), 8000)
        expected = []
        for k in range(length):
            at = k * step
            i = int(at)
            following = decoded[min(i + 1, count - 1)]
            expected.append(decoded[i] + (following - decoded[i]) * (at - i))
        start = first + int(float(seconds) * rate)
        best = (-2.0, 0)
        for lag in range(-2000, 2000):
            dot = power = heard = 0.0
            for k in range(0, length, 4):
                j = start + lag + k
                if 0 <= j < len(left):
                    dot += expected[k] * left[j]
                    power += expected[k] * expected[k]
                    heard += left[j] * left[j]
            if power and heard and dot / math.sqrt(power * heard) > best[0]:
                best = (dot / math.sqrt(power * heard), lag)
        correlation, lag = best
        at = start + lag
        part = left[at:at + length]
        gain = sum(e * h for e, h in zip(expected, part)) / max(1e-9, sum(e * e for e in expected))
        error = math.sqrt(sum((h - gain * e) ** 2 for e, h in zip(expected, part)) / length) or 1e-9
        signal = math.sqrt(sum((gain * e) ** 2 for e in expected) / length)
        print(f"  file {number}: at {at / rate:.3f} s, correlation {correlation:.4f}, gain {db(abs(gain) * 32768):+.1f} dB,"
              f" {20 * math.log10(max(signal, 1e-9) / error):.1f} dB apart")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture", type=Path)
    parser.add_argument("--wav", type=Path)
    parser.add_argument("--png", type=Path)
    parser.add_argument("--rate", type=int, default=RATE)
    parser.add_argument("--compare", nargs="+", metavar="ARG", help="<bank.dcs> <file>@<seconds> ...")
    args = parser.parse_args()
    raw = args.capture.read_bytes()
    samples = array("h")
    samples.frombytes(raw[:len(raw) // 4 * 4])
    left, right = samples[0::2], samples[1::2]
    rate = args.rate
    seconds = len(left) / rate
    print(f"{args.capture.name}: {seconds:.1f} s, {len(left)} frames")
    loud = 0
    for second in range(int(math.ceil(seconds))):
        a, b = second * rate, min(len(left), (second + 1) * rate)
        line = []
        for name, channel in (("L", left), ("R", right)):
            part = channel[a:b]
            if not part:
                continue
            peak = max(abs(x) for x in part)
            rms = math.sqrt(sum(x * x for x in part) / len(part))
            clipped = sum(1 for x in part if x >= 32767 or x <= -32768)
            dc = sum(part) / len(part)
            line.append(f"{name} peak {db(peak):6.1f} rms {db(rms):6.1f} dBFS, {clipped} clipped, dc {dc:+.0f}")
            loud += peak > 328
        print(f"  {second:3d}s  " + "; ".join(line))
    print(f"seconds with sound (a peak over -40 dBFS on a side): {loud // 2} or so of {int(math.ceil(seconds))}")
    if args.compare:
        compare(left, rate, Path(args.compare[0]), args.compare[1:])
    if args.wav:
        with wave.open(str(args.wav), "wb") as out:
            out.setnchannels(2)
            out.setsampwidth(2)
            out.setframerate(rate)
            out.writeframes(samples.tobytes())
    if args.png:
        from PIL import Image, ImageDraw
        width, height = 1600, 400
        image = Image.new("RGB", (width, height), (16, 16, 20))
        draw = ImageDraw.Draw(image)
        per = max(1, len(left) // width)
        for x in range(width):
            for row, channel, colour in ((0, left, (120, 200, 255)), (1, right, (255, 170, 110))):
                part = channel[x * per:(x + 1) * per]
                if not part:
                    continue
                top, bottom = max(part), min(part)
                middle = height // 4 + row * height // 2
                scale = (height // 4 - 4) / 32768
                draw.line([(x, middle - top * scale), (x, middle - bottom * scale)], fill=colour)
        for second in range(int(seconds) + 1):
            x = int(second * rate / per)
            draw.line([(x, 0), (x, 6)], fill=(200, 200, 200))
        image.save(args.png)


if __name__ == "__main__":
    main()
