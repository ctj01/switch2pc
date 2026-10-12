#!/usr/bin/env python3
"""Check a frame of examples/libnx_deko3d_triangle against an independent
reference: the deko_basic scene (clear to (0.125, 0.294, 0.478); a triangle
(0,1) red, (-1,-1) green, (1,-1) blue over a 1280x720 viewport) evaluated per
pixel centre with barycentrics. Pixels within one pixel of an edge are not
compared, since fill rules may legitimately differ there.

    check_triangle.py frame.ppm
"""
import sys


def read_ppm(path):
    data = open(path, "rb").read()
    # Header: four whitespace-separated tokens and exactly one whitespace byte before the
    # pixels (which may themselves start with bytes that look like whitespace).
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    if fields[0] != b"P6":
        raise SystemExit("not a binary PPM")
    w, h, maxval = int(fields[1]), int(fields[2]), int(fields[3])
    if maxval != 255:
        raise SystemExit("unexpected maxval")
    return w, h, data[pos: pos + w * h * 3]


def unorm8(v):
    v = min(max(v, 0.0), 1.0)
    return int(v * 255.0 + 0.5)


def main():
    w, h, px = read_ppm(sys.argv[1])
    if (w, h) != (1280, 720):
        raise SystemExit(f"frame is {w}x{h}, expected 1280x720")
    # window coordinates of the three vertices (viewport y is flipped)
    v = [(640.0, 0.0), (0.0, 720.0), (1280.0, 720.0)]
    col = [(1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)]
    bg = (unorm8(0.125), unorm8(0.294), unorm8(0.478))

    def edge(a, b, x, y):
        return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])

    area = edge(v[0], v[1], v[2][0], v[2][1])
    margin = 1.5 * max(abs(v[1][0] - v[0][0]), abs(v[1][1] - v[0][1]))  # ~1.5 px in edge units
    checked = bad = inside = 0
    worst = 0
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            cx, cy = x + 0.5, y + 0.5
            l0 = edge(v[1], v[2], cx, cy) / area
            l1 = edge(v[2], v[0], cx, cy) / area
            l2 = edge(v[0], v[1], cx, cy) / area
            near = min(abs(edge(v[1], v[2], cx, cy)), abs(edge(v[2], v[0], cx, cy)),
                       abs(edge(v[0], v[1], cx, cy))) < margin
            if near:
                continue
            if l0 >= 0 and l1 >= 0 and l2 >= 0:
                inside += 1
                exp = tuple(unorm8(l0 * col[0][c] + l1 * col[1][c] + l2 * col[2][c]) for c in range(3))
            else:
                exp = bg
            got = tuple(px[row + 3 * x: row + 3 * x + 3])
            d = max(abs(a - b) for a, b in zip(got, exp))
            worst = max(worst, d)
            checked += 1
            if d > 1:
                bad += 1
    print(f"triangle reference: {checked} pixels compared ({inside} inside), "
          f"{bad} differ by more than 1, worst difference {worst}")
    sys.exit(0 if bad == 0 and inside > 400000 else 1)


main()
