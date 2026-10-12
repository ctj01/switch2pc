#!/usr/bin/env python3
"""Check a frame of examples/libnx_deko3d_triangle against an independent
reference: the deko_basic scene (clear to (0.125, 0.294, 0.478); a triangle
(0,1) red, (-1,-1) green, (1,-1) blue over a 1280x720 viewport) evaluated per
pixel centre with barycentrics.

    check_triangle.py frame.ppm
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from frame_check import compare, unorm8  # noqa: E402


def main():
    win = [(640.0, 0.0), (0.0, 720.0), (1280.0, 720.0)]  # viewport y is flipped
    col = [(1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)]
    bg = (unorm8(0.125), unorm8(0.294), unorm8(0.478))
    checked, inside, bad, worst = compare(sys.argv[1], (1280, 720), bg,
                                          [(win, [1.0, 1.0, 1.0], col)], tolerance=1)
    print(f"triangle reference: {checked} pixels compared ({inside} inside), "
          f"{bad} differ by more than 1, worst difference {worst}")
    sys.exit(0 if bad == 0 and inside > 400000 else 1)


main()
