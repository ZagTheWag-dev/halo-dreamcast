"""Halo's boot intro for the Dreamcast (port/dc/src/dc_intro.c): the Xbox
disc's bink/intro.bik (the Microsoft and Bungie logos, 16 s) as

    intro/intro.dcv  the pictures: FRAME_RATE a second, each a VQ texture of
                     WIDTH x HEIGHT (KallistiOS's pvrtex: the codebook, then
                     the indices; no mipmaps), each on a sector of its own so
                     the disc reads them straight through
    intro/intro.dca  the sound: stereo at 44100 Hz in the AICA's 4-bit ADPCM,
                     the title music's format (menu_dc.py) with one version,
                     played once from RAM (it is 0.7 MB: the disc then only
                     reads the pictures, without seeking between two files)

ffmpeg reads the Bink file (FFMPEG, a full build: it decodes Bink).

    python -m tools.n64.intro_dc <intro.bik> --filesystem <file tree>
"""

import argparse
import glob
import os
import struct
import subprocess
import tempfile
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from .level_dc import FORMAT_RGB565, PVRTEX, PVRTEX_PATH
from .menu_dc import BLOCK_BYTES, BLOCK_SAMPLES, MUSIC_ENTRY, MUSIC_HEADER, MUSIC_MAGIC, MUSIC_VERSION, SECTOR
from .sounds_dc import aica_adpcm

FFMPEG = os.environ.get("FFMPEG") or next(iter(glob.glob("C:/Tools/ffmpeg/*/bin/ffmpeg.exe")), "ffmpeg")
FRAME_RATE = 15
WIDTH, HEIGHT = 512, 256
RATE = 44100

VIDEO_MAGIC = b"DCV1"
# magic, frame rate, frame count, width, height, PowerVR format word (VQ |
# RGB565 | twiddled), frame bytes (the texture), frame stride (sector
# padded), data offset
VIDEO_HEADER = struct.Struct("<4sIIHHIIII")
PVR_TXRFMT_RGB565_VQ = (1 << 27) | (1 << 30)


def vq_frame(png):
    """one picture as pvrtex's VQ texture data (no mipmaps)"""
    dt = png[:-4] + ".dt"
    env = dict(os.environ)
    env["PATH"] = PVRTEX_PATH + os.pathsep + env.get("PATH", "")
    done = subprocess.run([PVRTEX, "-i", png, "-o", dt, "-f", "RGB565", "-c"], capture_output=True, text=True,
                          env=env)
    if done.returncode:
        raise RuntimeError(f"pvrtex failed on {png}: {done.stdout[-300:]} {done.stderr[-300:]}")
    data = open(dt, "rb").read()
    if data[:4] != b"DcTx":
        raise ValueError("pvrtex wrote no .dt texture")
    return data[(data[9] + 1) * 32:struct.unpack_from("<I", data, 4)[0]]


def write_video(source, out):
    with tempfile.TemporaryDirectory() as work:
        subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-i", str(source), "-vf",
                        f"fps={FRAME_RATE},scale={WIDTH}:{HEIGHT}:flags=lanczos", os.path.join(work, "f%04d.png")],
                       check=True)
        pngs = sorted(glob.glob(os.path.join(work, "f*.png")))
        with ProcessPoolExecutor() as pool:
            frames = list(pool.map(vq_frame, pngs))
    size = len(frames[0])
    if any(len(f) != size for f in frames):
        raise ValueError("the frames' textures differ in size")
    stride = (size + SECTOR - 1) // SECTOR * SECTOR
    data = bytearray(SECTOR)
    VIDEO_HEADER.pack_into(data, 0, VIDEO_MAGIC, FRAME_RATE, len(frames), WIDTH, HEIGHT, PVR_TXRFMT_RGB565_VQ,
                           size, stride, SECTOR)
    for f in frames:
        data += f.ljust(stride, b"\0")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(data))
    return f"{len(frames)} pictures at {FRAME_RATE} a second, {size // 1024} KB each, {len(data) // 1024} KB"


def write_sound(source, out):
    pcm = subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-i", str(source), "-vn", "-ac", "2", "-ar",
                          str(RATE), "-f", "s16le", "-"], check=True, capture_output=True).stdout
    samples = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    left, right = aica_adpcm(samples[0::2]), aica_adpcm(samples[1::2])
    count = len(samples) // 2
    half = BLOCK_BYTES // 2
    blocks = (count + BLOCK_SAMPLES - 1) // BLOCK_SAMPLES
    body = bytearray()
    for b in range(blocks):
        body += left[b * half:(b + 1) * half].ljust(half, b"\0")
        body += right[b * half:(b + 1) * half].ljust(half, b"\0")
    head = bytearray(SECTOR)
    # (one version, its loop the same: dc_intro.c plays it once)
    MUSIC_HEADER.pack_into(head, 0, MUSIC_MAGIC, MUSIC_VERSION, RATE, 1, 0, BLOCK_BYTES, SECTOR)
    MUSIC_ENTRY.pack_into(head, MUSIC_HEADER.size, 0, blocks, count)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(head + body))
    return f"{count / RATE:.1f} s of sound, {(len(head) + len(body)) // 1024} KB"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("bik", type=Path)
    parser.add_argument("--filesystem", type=Path, required=True)
    args = parser.parse_args()
    video = args.filesystem / "intro" / "intro.dcv"
    sound = args.filesystem / "intro" / "intro.dca"
    newest = max(args.bik.stat().st_mtime, Path(__file__).stat().st_mtime)
    if video.is_file() and sound.is_file() and min(video.stat().st_mtime, sound.stat().st_mtime) > newest:
        print("intro as it was")
        return
    print("intro: " + write_sound(args.bik, sound))
    print("intro: " + write_video(args.bik, video))


if __name__ == "__main__":
    main()
