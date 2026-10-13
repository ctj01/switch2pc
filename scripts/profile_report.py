#!/usr/bin/env python3
"""Turn a HOS_PROFILE dump into a function and source-line profile.

    HOS_PROFILE=/tmp/prof.txt qemu-aarch64 ... hosrun module.nso
    scripts/profile_report.py /tmp/prof.txt [top]

Samples in mapped files are resolved with aarch64-linux-gnu-addr2line (or
addr2line); anonymous mappings (the loaded NSO, JIT-free guest code) are
reported as [module] with their address.
"""
import collections
import shutil
import subprocess
import sys


def parse(path):
    maps, pcs = [], []
    with open(path) as f:
        for line in f:
            if line.startswith("map "):
                parts = line[4:].split()
                lo, hi = (int(x, 16) for x in parts[0].split("-"))
                off = int(parts[2], 16)
                name = parts[5] if len(parts) > 5 else ""
                maps.append((lo, hi, off, name))
            elif line.startswith("pc "):
                _, pc, n = line.split()
                pcs.append((int(pc, 16), int(n)))
    return maps, pcs


def main():
    maps, pcs = parse(sys.argv[1])
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    total = sum(n for _, n in pcs)
    if not total:
        raise SystemExit("no samples")
    by_file = collections.defaultdict(list)  # file -> [(offset, count)]
    other = collections.Counter()
    for pc, n in pcs:
        m = next((m for m in maps if m[0] <= pc < m[1]), None)
        if m and m[3].startswith("/"):
            # shared objects: text vaddr equals file offset, so the file offset is the address
            by_file[m[3]].append((pc - m[0] + m[2], n))
        else:
            other[f"[{m[3] or 'anon'}] {pc:#x}" if m else f"[unmapped] {pc:#x}"] += n
    a2l = shutil.which("aarch64-linux-gnu-addr2line") or "addr2line"
    funcs = collections.Counter()  # outermost function (inlined callees fold into it)
    lines = collections.Counter()  # innermost source line
    libs = collections.Counter()
    for path, items in by_file.items():
        lib = path.rsplit("/", 1)[-1]
        libs[lib] += sum(n for _, n in items)
        # -i prints the inline chain for each address; a sentinel address marks the end
        args = []
        for o, _ in items:
            args += [hex(o), "0x0"]
        out = subprocess.run([a2l, "-f", "-i", "-e", path] + args, capture_output=True,
                             text=True).stdout.split("\n")
        k = 0
        for _, n in items:
            chain = []
            while k + 1 < len(out) and not (out[k] == "??" and out[k + 1].startswith("??:")):
                chain.append((out[k], out[k + 1]))
                k += 2
            k += 2  # the sentinel's lines
            if not chain:
                chain = [("??", "??:0")]
            funcs[f"{lib}: {chain[-1][0]}"] += n
            lines[f"{chain[0][1].rsplit('/', 1)[-1]} ({chain[0][0]})"] += n
    print(f"{total} samples")
    print("by object:")
    for lib, n in libs.most_common(10):
        print(f"  {100 * n / total:5.1f}%  {lib}")
    rest = sum(other.values())
    if rest:
        print(f"  {100 * rest / total:5.1f}%  (anonymous: guest module code)")
    print("by function:")
    for name, n in (funcs + other).most_common(top):
        print(f"  {100 * n / total:5.1f}%  {name}")
    print("by source line:")
    for name, n in lines.most_common(top):
        print(f"  {100 * n / total:5.1f}%  {name}")


main()
