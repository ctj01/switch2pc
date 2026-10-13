#!/usr/bin/env python3
"""Check a frame of deko_examples 05 (tessellation) against an independent
reference built from the OpenGL 4.6 rules (section 11.2.2.2): the control
shader asks for inner level 5 and outer levels 2, 3, 5 over the triangle
(0,1) (-1,-1) (1,-1), equal spacing, drawn in line polygon mode 4 pixels wide
on a 1280x720 viewport.

The rings are fixed by the specification: concentric triangles with parallel
edges, every segment one n-th of an edge, ring k's corners at coordinate
1 - 4k/3n; so every ring edge must be drawn. How the band between two rings
is triangulated is implementation-defined, so lit pixels may also lie on any
segment joining a point of an outer edge to a point of the facing inner edge,
and nowhere else.

    check_tess.py frame.ppm
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from frame_check import read_ppm  # noqa: E402

WIDTH, HEIGHT = 1280, 720
PATCH = [(0.0, 1.0), (-1.0, -1.0), (1.0, -1.0)]  # NDC, as the example's vertex buffer
INNER, OUTER = 5, (2, 3, 5)
STRAY_RADIUS = 3.5  # half the line width, plus the pixel centre's offset
MISSING_RADIUS = 2.0


def window(bary):
    """Window coordinates of a barycentric point (u, v, w); the viewport flips y."""
    x = sum(b * p[0] for b, p in zip(bary, PATCH))
    y = sum(b * p[1] for b, p in zip(bary, PATCH))
    return ((x + 1.0) / 2.0 * WIDTH, (1.0 - y) / 2.0 * HEIGHT)


def ring_corner(k, n, c):
    lo = 2.0 * k / (3.0 * n)
    p = [lo, lo, lo]
    p[c] = 1.0 - 2.0 * lo
    return p


def ring_edges(n, outer):
    """Per ring, per edge: its points in order. Ring 0's edges carry the outer
    levels; ring k's carry n - 2k segments; a ring of 0 segments is the centre."""
    rings = []
    for k in range(n // 2 + 1):
        edges = []
        for e in range(3):
            m = outer[e] if k == 0 else n - 2 * k
            if m == 0:
                edges.append([window((1 / 3, 1 / 3, 1 / 3))])
                continue
            a, c = ring_corner(k, n, (e + 1) % 3), ring_corner(k, n, (e + 2) % 3)
            pts = []
            for i in range(m + 1):
                t = i / m
                pts.append(window([a[j] + (c[j] - a[j]) * t for j in range(3)]))
            edges.append(pts)
        rings.append(edges)
    return rings


def segments(n, outer):
    rings = ring_edges(n, outer)
    required, allowed = [], []
    for k, edges in enumerate(rings):
        for pts in edges:
            for i in range(len(pts) - 1):
                required.append((pts[i], pts[i + 1]))
        if k + 1 < len(rings):
            for e in range(3):
                for p in edges[e]:
                    for q in rings[k + 1][e]:
                        allowed.append((p, q))
    return required, required + allowed


def dist_to_segment(p, a, b):
    ax, ay = a
    dx, dy = b[0] - ax, b[1] - ay
    l2 = dx * dx + dy * dy
    t = 0.0 if l2 == 0.0 else max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy) / l2))
    return math.hypot(p[0] - (ax + t * dx), p[1] - (ay + t * dy))


def main():
    w, h, px = read_ppm(sys.argv[1])
    if (w, h) != (WIDTH, HEIGHT):
        raise SystemExit(f"unexpected frame size {w}x{h}")
    lit = set()
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            i = row + 3 * x
            if px[i] > 32 or px[i + 1] > 32 or px[i + 2] > 32:
                lit.add((x, y))
    required, allowed = segments(INNER, OUTER)
    # Lit pixels must sit on an allowed segment: mark the pixels each one covers.
    covered = set()
    r = int(math.ceil(STRAY_RADIUS))
    for a, b in allowed:
        steps = int(math.hypot(b[0] - a[0], b[1] - a[1])) + 1
        for s in range(steps + 1):
            t = s / steps
            cx, cy = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
            for y in range(int(cy) - r, int(cy) + r + 1):
                for x in range(int(cx) - r, int(cx) + r + 1):
                    if (x, y) not in covered and (x, y) in lit and \
                            dist_to_segment((x + 0.5, y + 0.5), a, b) <= STRAY_RADIUS:
                        covered.add((x, y))
    stray = len(lit - covered)
    # Every ring edge must be drawn: sample it away from the corners.
    samples = missing = 0
    for a, b in required:
        length = math.hypot(b[0] - a[0], b[1] - a[1])
        s = 4.0
        while s < length - 4.0:
            t = s / length
            cx, cy = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
            samples += 1
            if not any((int(cx) + dx, int(cy) + dy) in lit
                       for dy in range(-2, 3) for dx in range(-2, 3)
                       if math.hypot(dx, dy) <= MISSING_RADIUS):
                missing += 1
            s += 2.0
    print(f"tessellation reference: {len(lit)} lit pixels, {stray} stray, "
          f"{missing} missing of {samples} ring edge samples")
    sys.exit(0 if stray == 0 and missing == 0 and len(lit) > 10000 else 1)


main()
