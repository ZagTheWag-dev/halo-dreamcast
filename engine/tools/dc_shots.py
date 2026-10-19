#!/usr/bin/env python3
"""Crops Flycast window captures (tools/dc_flycast_test.ps1) to the 4:3
picture (the window may be any size: the picture is pillarboxed) and lays
several out in a grid.

    python tools/dc_shots.py out.png shot-0.png shot-1.png ...
"""

import sys

from PIL import Image


def picture(path, size=(480, 360)):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    cols = [x for x in range(0, w, 4) if any(sum(im.getpixel((x, y))) > 30 for y in range(0, h, 16))]
    if cols:
        im = im.crop((cols[0], 0, cols[-1] + 1, h))
    return im.resize(size)


def main():
    out, paths = sys.argv[1], sys.argv[2:]
    per_row = 3 if len(paths) > 4 else 2 if len(paths) > 1 else 1
    rows = (len(paths) + per_row - 1) // per_row
    grid = Image.new("RGB", (480 * per_row, 360 * rows))
    for i, p in enumerate(paths):
        grid.paste(picture(p), ((i % per_row) * 480, (i // per_row) * 360))
    grid.save(out)


if __name__ == "__main__":
    main()
