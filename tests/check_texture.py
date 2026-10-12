#!/usr/bin/env python3
"""Check a frame of examples/libnx_deko3d_texture against an independent
reference: the same 64x64 texture (red 4x, green 4y, blue on an 8x8
checkerboard), sampled at every pixel centre with OpenGL's rules: nearest
with clamp-to-edge on the left quad (uv 0..1), bilinear with repeat on the
right (uv 0..2); and a 16x16 BC1 texture on the bottom quad (nearest,
clamp), decoded here from the format's definition. Pixels within 1.5 px of
a quad edge are not compared.

    check_texture.py frame.ppm
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from frame_check import read_ppm, unorm8  # noqa: E402

W, H, N = 1280, 720, 64


def texel(x, y):
    return (x * 4 / 255, y * 4 / 255, 1.0 if (x // 8 + y // 8) & 1 else 0.0, 1.0)


def rgb565(r5, g6, b5):
    return (r5 << 11) | (g6 << 5) | b5


def expand565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3 | r >> 2) / 255, (g << 2 | g >> 4) / 255, (b << 3 | b >> 2) / 255)


def bc1_texel(x, y):
    bx, by, tx, ty = x // 4, y // 4, x % 4, y % 4
    hi, lo = rgb565(31, by * 21, bx * 10), rgb565(2, 5, 31 - bx * 7)
    c0, c1 = (lo, hi) if (bx + by) & 1 else (hi, lo)
    idx = (tx + 2 * ty + bx) & 3
    e0, e1 = expand565(c0), expand565(c1)
    if idx == 0:
        return (*e0, 1.0)
    if idx == 1:
        return (*e1, 1.0)
    if c0 > c1:
        w = (2, 1) if idx == 2 else (1, 2)
        return (*[(w[0] * a + w[1] * b) / 3 for a, b in zip(e0, e1)], 1.0)
    if idx == 2:
        return (*[(a + b) / 2 for a, b in zip(e0, e1)], 1.0)
    return (0.0, 0.0, 0.0, 0.0)


def wrap(i, mode, n=N):
    return i % n if mode == "repeat" else min(max(i, 0), n - 1)


def sample(u, v, linear, mode, tex=texel, n=N):
    if not linear:
        return tex(wrap(math.floor(u * n), mode, n), wrap(math.floor(v * n), mode, n))
    fx, fy = u * N - 0.5, v * N - 0.5
    x0, y0 = math.floor(fx), math.floor(fy)
    ax, ay = fx - x0, fy - y0
    t = [[texel(wrap(x0 + dx, mode), wrap(y0 + dy, mode)) for dx in (0, 1)] for dy in (0, 1)]
    return tuple((t[0][0][c] * (1 - ax) + t[0][1][c] * ax) * (1 - ay) +
                 (t[1][0][c] * (1 - ax) + t[1][1][c] * ax) * ay for c in range(4))


# (x0, y0, x1, y1) in NDC, uv scale, linear, wrap
QUADS = [((-0.9, -0.4, -0.1, 0.4), 1.0, False, "clamp", texel, N),
         ((0.1, -0.4, 0.9, 0.4), 2.0, True, "repeat", texel, N),
         ((-0.4, -0.95, 0.4, -0.55), 1.0, False, "clamp", bc1_texel, 16)]


def main():
    w, h, px = read_ppm(sys.argv[1])
    if (w, h) != (W, H):
        raise SystemExit(f"frame is {w}x{h}")
    rects = []
    for (x0, y0, x1, y1), scale, linear, mode, tex, n in QUADS:
        rects.append(((x0 + 1) * W / 2, (1 - y1) * H / 2, (x1 + 1) * W / 2, (1 - y0) * H / 2,
                      scale, linear, mode, tex, n))
    bg = (unorm8(0.1),) * 3
    checked = bad = inside = worst = 0
    for y in range(H):
        cy = y + 0.5
        for x in range(W):
            cx = x + 0.5
            exp = bg
            skip = False
            for (rx0, ry0, rx1, ry1, scale, linear, mode, tex, n) in rects:
                near_x = min(abs(cx - rx0), abs(cx - rx1)) < 1.5 and ry0 - 1.5 <= cy <= ry1 + 1.5
                near_y = min(abs(cy - ry0), abs(cy - ry1)) < 1.5 and rx0 - 1.5 <= cx <= rx1 + 1.5
                if near_x or near_y:
                    skip = True
                elif rx0 <= cx < rx1 and ry0 <= cy < ry1:
                    u = (cx - rx0) / (rx1 - rx0) * scale
                    v = (cy - ry0) / (ry1 - ry0) * scale
                    exp = tuple(unorm8(c) for c in sample(u, v, linear, mode, tex, n)[:3])
                    inside += 1
            if skip:
                continue
            got = tuple(px[(y * W + x) * 3: (y * W + x) * 3 + 3])
            d = max(abs(a - b) for a, b in zip(got, exp))
            worst = max(worst, d)
            checked += 1
            if d > 1:
                bad += 1
    print(f"texture reference: {checked} pixels compared ({inside} textured), {bad} differ by "
          f"more than 1, worst difference {worst}")
    sys.exit(0 if bad == 0 and inside > 350000 else 1)


main()
