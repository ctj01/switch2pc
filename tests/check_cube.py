#!/usr/bin/env python3
"""Check a frame of examples/libnx_deko3d_cube against an independent
reference: the same cube, transform and camera computed here in double
precision (Rodrigues rotations, a perspective matrix with depth 0..1, the
1280x720 viewport with Y flipped), back faces culled, quads split along
their 0-2 diagonal, colours interpolated perspective-correctly.

    check_cube.py frame.ppm
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from frame_check import compare  # noqa: E402

CUBE = [
    ((+1, +1, +1), (1, 0, 0)), ((+1, -1, +1), (0, 1, 0)), ((+1, -1, -1), (0, 0, 1)),
    ((+1, +1, -1), (1, 1, 0)), ((-1, +1, -1), (1, 0, 0)), ((-1, -1, -1), (0, 1, 0)),
    ((-1, -1, +1), (0, 0, 1)), ((-1, +1, +1), (1, 1, 0)), ((-1, +1, -1), (1, 0, 0)),
    ((-1, +1, +1), (0, 1, 0)), ((+1, +1, +1), (0, 0, 1)), ((+1, +1, -1), (1, 1, 0)),
    ((-1, -1, +1), (1, 0, 0)), ((-1, -1, -1), (0, 1, 0)), ((+1, -1, -1), (0, 0, 1)),
    ((+1, -1, +1), (1, 1, 0)), ((-1, +1, +1), (1, 0, 0)), ((-1, -1, +1), (0, 1, 0)),
    ((+1, -1, +1), (0, 0, 1)), ((+1, +1, +1), (1, 1, 0)), ((+1, +1, -1), (1, 0, 0)),
    ((+1, -1, -1), (0, 1, 0)), ((-1, -1, -1), (0, 0, 1)), ((-1, +1, -1), (1, 1, 0)),
]
PERIOD1, PERIOD2 = 0.1, 0.15
W, H = 1280, 720


def matmul(a, b):  # row-major 4x4
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def rot(angle, axis):
    x, y, z = axis
    c, s, k = math.cos(angle), math.sin(angle), 1 - math.cos(angle)
    return [[c + k * x * x, k * x * y - s * z, k * x * z + s * y, 0],
            [k * y * x + s * z, c + k * y * y, k * y * z - s * x, 0],
            [k * z * x - s * y, k * z * y + s * x, c + k * z * z, 0],
            [0, 0, 0, 1]]


def main():
    tau = 2 * math.pi
    trans = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, -3], [0, 0, 0, 1]]
    scale = [[0.5, 0, 0, 0], [0, 0.5, 0, 0], [0, 0, 0.5, 0], [0, 0, 0, 1]]
    mv = matmul(matmul(matmul(trans, rot(math.sin(PERIOD2 * tau) * tau / 8, (1, 0, 0))),
                       rot(-PERIOD1 * tau, (0, 1, 0))), scale)
    n, f, t = 0.01, 1000.0, math.tan(math.radians(40) / 2)
    proj = [[1 / ((W / H) * t), 0, 0, 0], [0, 1 / t, 0, 0],
            [0, 0, f / (n - f), -(f * n) / (f - n)], [0, 0, -1, 0]]
    mvp = matmul(proj, mv)

    verts = []
    for (p, c) in CUBE:
        clip = [sum(mvp[i][j] * v for j, v in enumerate((*p, 1))) for i in range(4)]
        inv_w = 1 / clip[3]
        win = (clip[0] * inv_w * (W / 2) + W / 2, clip[1] * inv_w * -(H / 2) + H / 2)
        verts.append((win, inv_w, c))

    tris = []
    for q in range(0, 24, 4):
        for a, b, c in ((q, q + 1, q + 2), (q, q + 2, q + 3)):
            v = [verts[a], verts[b], verts[c]]
            win = [x[0] for x in v]
            area = (win[1][0] - win[0][0]) * (win[2][1] - win[0][1]) - \
                   (win[1][1] - win[0][1]) * (win[2][0] - win[0][0])
            if area >= 0:  # window y points down: counter-clockwise in NDC is negative here
                continue
            tris.append((win, [x[1] for x in v], [x[2] for x in v]))

    checked, inside, bad, worst = compare(sys.argv[1], (W, H), (0, 0, 0), tris, tolerance=2)
    print(f"cube reference: {len(tris)} front triangles, {checked} pixels compared "
          f"({inside} inside), {bad} differ by more than 2, worst difference {worst}")
    sys.exit(0 if bad == 0 and len(tris) == 6 and inside > 150000 else 1)


main()
