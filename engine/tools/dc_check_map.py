#!/usr/bin/env python3
"""Checks a Dreamcast map (tools/n64/convert.py --dc) before it goes on a
disc: the header the game tests (port/n64/game/n64_game.c
n64_map_header_compatible), the 'tags'/'sbsp' signatures in the SH-4's
order, and every relocation (4-byte aligned, inside the image, pointing
inside the image). The SH-4 traps on an unaligned 32-bit access, so one bad
relocation would crash the game at load.

    python tools/dc_check_map.py build/dc/fs/maps/bloodgulch.map
"""

import struct
import sys
from pathlib import Path

VERSION = 2
KIND_MULTIPLAYER = 0x4D503332


def check(path):
    data = Path(path).read_bytes()
    problems = []
    if data[:4] != b"H64M":
        return [f"magic {data[:4]!r}, not H64M"]
    version, = struct.unpack_from("<I", data, 4)
    name = data[8:40].split(b"\0")[0].decode(errors="replace")
    image_size, relocation_count, tag_header, bsp_header, kind, reserved = struct.unpack_from("<6I", data, 40)
    if version != VERSION:
        problems.append(f"version {version:#x} (big-endian map? the N64's reads 0x02000000 here)")
    if kind != KIND_MULTIPLAYER or reserved != 0:
        problems.append(f"kind {kind:#x}/{reserved:#x}, not MP32/0")
    image = data[64:64 + image_size]
    if len(image) != image_size:
        problems.append(f"image is {len(image)} bytes, header says {image_size}")
    if image[tag_header + 32:tag_header + 36] != b"sgat":
        problems.append(f"tag header signature {image[tag_header + 32:tag_header + 36]!r}, not 'tags' in the SH-4's order")
    if image[bsp_header + 20:bsp_header + 24] != b"psbs":
        problems.append(f"structure BSP signature {image[bsp_header + 20:bsp_header + 24]!r}, not 'sbsp' in the SH-4's order")
    start = (64 + image_size + 7) & ~7
    relocations = struct.unpack_from(f"<{relocation_count}I", data, start)
    if start + 4 * relocation_count != len(data):
        problems.append(f"{len(data) - start - 4 * relocation_count} bytes after the relocations")
    unaligned = [r for r in relocations if r % 4]
    outside = [r for r in relocations if r > image_size - 4]
    if unaligned:
        problems.append(f"{len(unaligned)} unaligned relocations, first at {unaligned[0]:#x}")
    if outside:
        problems.append(f"{len(outside)} relocations outside the image, first {outside[0]:#x}")
    wild = [r for r in relocations if r % 4 == 0 and r <= image_size - 4
            and struct.unpack_from("<I", image, r)[0] >= image_size]
    if wild:
        r = wild[0]
        problems.append(f"{len(wild)} pointers past the image, first at {r:#x} -> {struct.unpack_from('<I', image, r)[0]:#x}")
    print(f"{name}: {image_size / 1024:.0f} KB image, {relocation_count} relocations, "
          f"tag header at {tag_header:#x}, BSP header at {bsp_header:#x}")
    return problems


def main():
    failed = False
    for path in sys.argv[1:]:
        problems = check(path)
        for p in problems:
            print(f"{path}: {p}")
        failed |= bool(problems)
        if not problems:
            print(f"{path}: OK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
