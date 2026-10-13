#!/usr/bin/env python3
"""Compare dumped frames with references of the same name.

    compare_frames.py <frames dir> <reference dir> [tolerance]

Every frame_*.ppm in the reference directory must exist among the dumped
frames, with the same size and no channel differing by more than `tolerance`
(default 2). Prints one line per frame; exits 1 on any mismatch.
"""
import glob
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))
from frame_check import read_ppm  # noqa: E402


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    frames, refs = sys.argv[1], sys.argv[2]
    tol = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    names = sorted(os.path.basename(p) for p in glob.glob(os.path.join(refs, "frame_*.ppm")))
    if not names:
        print(f"no reference frames in {refs}")
        return 1
    bad = 0
    for name in names:
        got_path = os.path.join(frames, name)
        if not os.path.exists(got_path):
            print(f"{name}: missing (the app presented fewer frames?)")
            bad += 1
            continue
        w0, h0, ref = read_ppm(os.path.join(refs, name))
        w1, h1, got = read_ppm(got_path)
        if (w0, h0) != (w1, h1):
            print(f"{name}: {w1}x{h1}, reference {w0}x{h0}")
            bad += 1
            continue
        diff = [abs(a - b) for a, b in zip(got, ref)]
        worst = max(diff) if diff else 0
        off = sum(1 for d in diff if d > tol)
        ok = off == 0
        print(f"{name}: {'ok' if ok else 'DIFFERS'}, worst channel difference {worst}, "
              f"{off} channels beyond {tol}")
        bad += not ok
    print(f"{len(names) - bad}/{len(names)} frames match")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
