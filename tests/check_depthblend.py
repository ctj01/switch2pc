#!/usr/bin/env python3
"""Check a frame of examples/libnx_deko3d_depthblend against an independent
reference: the four quads drawn in order into an 8-bit RGBA colour buffer
and a unorm24 depth buffer cleared to 1.0, with depth test LESS, depth write
on or off per draw, and SrcAlpha / InvSrcAlpha blending reading the stored
8-bit destination. Pixels within 1.5 px of a quad edge are not compared.

    check_depthblend.py frame.ppm
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from frame_check import read_ppm, unorm8  # noqa: E402

W, H = 1280, 720
# (x0, y0, x1, y1, z, rgba, depth_write, blend)
DRAWS = [
    (-0.6, -0.5, 0.2, 0.5, 0.3, (1, 0, 0, 1), True, False),
    (-0.2, -0.3, 0.6, 0.7, 0.6, (0, 1, 0, 1), True, False),
    (0.0, -0.7, 0.8, 0.1, 0.1, (0, 0, 1, 0.5), False, True),
    (-0.9, -0.9, 0.9, -0.6, 0.9, (1, 1, 0, 1), True, False),
]


def win_rect(d):
    x0, y0, x1, y1 = d[:4]
    return (x0 + 1) * W / 2, (1 - y1) * H / 2, (x1 + 1) * W / 2, (1 - y0) * H / 2


def main():
    w, h, px = read_ppm(sys.argv[1])
    if (w, h) != (W, H):
        raise SystemExit(f"frame is {w}x{h}")
    rects = [win_rect(d) for d in DRAWS]
    checked = bad = worst = 0
    for y in range(H):
        cy = y + 0.5
        for x in range(W):
            cx = x + 0.5
            if any(min(abs(cx - r[0]), abs(cx - r[2])) < 1.5 and r[1] - 1.5 <= cy <= r[3] + 1.5 or
                   min(abs(cy - r[1]), abs(cy - r[3])) < 1.5 and r[0] - 1.5 <= cx <= r[2] + 1.5
                   for r in rects):
                continue
            color = [unorm8(0.1), unorm8(0.1), unorm8(0.1), 255]
            depth = 0xFFFFFF
            for d, r in zip(DRAWS, rects):
                if not (r[0] <= cx < r[2] and r[1] <= cy < r[3]):
                    continue
                zq = int(d[4] * 16777215 + 0.5)
                if not zq < depth:
                    continue
                src = d[5]
                if d[7]:
                    dst = [c / 255 for c in color]
                    a = src[3]
                    out = [src[c] * a + dst[c] * (1 - a) for c in range(3)]
                    out.append(src[3] * 1 + dst[3] * (1 - a))  # alpha: One / InvSrcAlpha
                    color = [unorm8(v) for v in out]
                else:
                    color = [unorm8(v) for v in src]
                if d[6]:
                    depth = zq
            got = tuple(px[(y * W + x) * 3: (y * W + x) * 3 + 3])
            diff = max(abs(a - b) for a, b in zip(got, color[:3]))
            worst = max(worst, diff)
            checked += 1
            if diff > 1:
                bad += 1
    print(f"depth/blend reference: {checked} pixels compared, {bad} differ by more than 1, "
          f"worst difference {worst}")
    sys.exit(0 if bad == 0 and checked > 900000 else 1)


main()
