"""Shared helpers for the frame checks (check_triangle.py, check_cube.py):
reading hosrun's PPM dumps, and a reference rasteriser for triangles with
per-vertex colours, evaluated in double precision at pixel centres."""


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


def edge(a, b, x, y):
    return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])


def compare(path, size, background, triangles, tolerance=1, min_inside=1):
    """triangles: list of (win[3] as (x, y), inv_w[3], colour[3] as (r, g, b)).
    Colours are interpolated perspective-correctly; pixels within 1.5 px of any
    triangle edge are skipped, since fill rules may legitimately differ there."""
    w, h, px = read_ppm(path)
    if (w, h) != size:
        raise SystemExit(f"frame is {w}x{h}, expected {size[0]}x{size[1]}")
    expected = {}
    near = set()
    for win, inv_w, col in triangles:
        area = edge(win[0], win[1], win[2][0], win[2][1])
        if area == 0:
            continue
        xs = [p[0] for p in win]
        ys = [p[1] for p in win]
        x0, x1 = max(int(min(xs)) - 2, 0), min(int(max(xs)) + 3, w)
        y0, y1 = max(int(min(ys)) - 2, 0), min(int(max(ys)) + 3, h)
        lens = [((win[(i + 1) % 3][0] - win[i][0]) ** 2 + (win[(i + 1) % 3][1] - win[i][1]) ** 2) ** 0.5
                for i in range(3)]
        for y in range(y0, y1):
            for x in range(x0, x1):
                cx, cy = x + 0.5, y + 0.5
                e = [edge(win[1], win[2], cx, cy), edge(win[2], win[0], cx, cy),
                     edge(win[0], win[1], cx, cy)]
                dist = [abs(e[0]) / lens[1], abs(e[1]) / lens[2], abs(e[2]) / lens[0]]
                l = [ei / area for ei in e]
                inside = all(li >= 0 for li in l)
                if min(dist) < 1.5 and (inside or min(dist) < 1.5):
                    near.add((x, y))
                    continue
                if not inside:
                    continue
                q = [l[i] * inv_w[i] for i in range(3)]
                s = sum(q)
                expected[(x, y)] = tuple(
                    unorm8(sum(q[i] * col[i][c] for i in range(3)) / s) for c in range(3))
    checked = bad = inside_count = worst = 0
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            if (x, y) in near:
                continue
            exp = expected.get((x, y))
            if exp is None:
                exp = background
            else:
                inside_count += 1
            got = tuple(px[row + 3 * x: row + 3 * x + 3])
            d = max(abs(a - b) for a, b in zip(got, exp))
            worst = max(worst, d)
            checked += 1
            if d > tolerance:
                bad += 1
    return checked, inside_count, bad, worst
