#!/usr/bin/env python3
"""Generate docs/status.html, the project's progress page, from the sources.

    scripts/gen_status.py            # writes docs/status.html, prints a summary

What it measures:
  - Kernel calls: the SVC table (shims/hos_cpu/svc_table.c), implemented
    against known.
  - GPU shader instructions: every Maxwell encoding envytools knows
    (docs/data/maxwell_isa.tsv) against the patterns the interpreter decodes
    (is(i, value, mask) in shims/hos_gpu/gpu_shader*).
  - IPC: commands handled by each service interface (case labels in its
    dispatch function), with no total, since HOS defines thousands.
  - Homebrew: docs/data/compat.json, kept by hand.
  - History: kernel-call and shader coverage at every commit, from git.
"""
import collections
import datetime
import html
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHADER_FILES = ["shims/hos_gpu/gpu_shader.c", "shims/hos_gpu/gpu_shader_simd.c",
                "shims/hos_gpu/gpu_shader_impl.h"]

GREEN, GRAY, YELLOW, RED = "#2ea043", "#6e7681", "#d29922", "#da3633"


def read(path, rev=None):
    if rev is None:
        try:
            with open(os.path.join(ROOT, path)) as f:
                return f.read()
        except OSError:
            return None
    r = subprocess.run(["git", "-C", ROOT, "show", f"{rev}:{path}"], capture_output=True)
    return r.stdout.decode("utf-8", "replace") if r.returncode == 0 else None


# ---- kernel calls --------------------------------------------------------------------

SVC_RE = re.compile(r'\[(0x[0-9A-Fa-f]+)\]\s*=\s*\{\s*"(\w+)"\s*,\s*(\w+)\s*\}')
SVC_GROUPS = [
    ("IPC", ("Session", "Request", "Reply", "Port", "Accept", "Connect", "Light")),
    ("Synchronization", ("Event", "Signal", "Wait", "Arbitrate", "Mutex", "Condition", "Lock",
                         "Synchronization", "Cancel")),
    ("Threads", ("Thread", "Sleep", "Core", "Priority", "Yield")),
    ("Memory", ("Heap", "Memory", "Map", "Unmap", "Physical", "Transfer", "Code", "Shared",
                "Insecure", "Io", "Region")),
    ("Debug", ("Debug", "Break", "Output", "Trace", "Dump", "Process")),
    ("System", ("Info", "Resource", "Tick", "System", "Kernel", "Interrupt", "Device", "Call",
                "Secure", "Smc", "Pool", "Return", "Exit", "Close", "Flush", "Invalidate",
                "Store", "Barrier")),
]


def svcs(text):
    out = []
    for num, name, fn in SVC_RE.findall(text or ""):
        group = next((g for g, keys in SVC_GROUPS if any(k in name for k in keys)), "Other")
        out.append((int(num, 16), name, fn != "NULL", group))
    return out


# ---- shader instructions -------------------------------------------------------------

def isa():
    rows = []
    with open(os.path.join(ROOT, "docs/data/maxwell_isa.tsv")) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            v, m, name = line.split()
            rows.append((int(v, 16), int(m, 16), name))
    return rows


ISA_FAMILIES = [
    ("Control", {"bra", "brx", "jmp", "jmx", "cal", "jcal", "ret", "exit", "ssy", "sync", "pbk",
                 "brk", "pcnt", "cont", "kil", "nop", "bpt", "rtt", "pret", "pexit", "plongjmp",
                 "longjmp", "lepc", "vote", "bar", "depbar", "membar", "sam", "ram", "ide",
                 "stp"}),
    ("Texture", {"tex", "texs", "tld", "tlds", "tld4", "tld4s", "txq", "txd", "tmml", "txa"}),
    ("Memory", {"ld", "st", "ldg", "stg", "lds", "sts", "ldl", "stl", "atom", "atoms", "red",
                "cctl", "cctll", "cctlt", "al2p", "isberd", "out", "pixld", "shfl", "suld",
                "sust", "sured", "suatom", "ipa"}),
    ("Move and convert", {"mov", "mov32i", "s2r", "cs2r", "r2p", "p2r", "b2r", "r2b", "f2i",
                          "i2f", "i2i", "f2f", "rro", "mufu", "sel", "prmt", "fswzadd"}),
    ("Predicates", {"psetp", "pset", "csetp", "cset", "fsetp", "fset", "isetp", "iset",
                    "dsetp", "dset", "hsetp2", "hset2", "fcmp", "icmp", "fchk", "vsetp", "vset"}),
    ("Float", None),  # f*, d*, h* arithmetic
    ("Integer", None),
]


def isa_family(name):
    for fam, names in ISA_FAMILIES:
        if names and name in names:
            return fam
    if name[0] in "fdh":
        return "Float"
    return "Integer"


PATTERN_RE = re.compile(r"is\(\s*i\s*,\s*(0x[0-9a-fA-F]+)ull\s*,\s*(0x[0-9a-fA-F]+)ull\s*\)")


def shader_patterns(rev=None):
    pats = set()
    for path in SHADER_FILES:
        for v, m in PATTERN_RE.findall(read(path, rev) or ""):
            pats.add((int(v, 16), int(m, 16)))
    return pats


def covered(entry, pats):
    v, m, _ = entry
    return any(((v ^ pv) & m & pm) == 0 for pv, pm in pats)


# ---- IPC -----------------------------------------------------------------------------

CLASS_RE = re.compile(r"hos_service_class\s+\w+\s*=\s*\{(.*?)\};", re.S)


def function_body(src, name):
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", src)
    if not m:
        return ""
    depth, i = 1, m.end()
    while i < len(src) and depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    return src[m.end():i]


def ipc():
    lib_ifaces = collections.defaultdict(list)  # shim -> [(interface, commands)]
    shims = os.path.join(ROOT, "shims")
    for lib in sorted(os.listdir(shims)):
        d = os.path.join(shims, lib)
        if not os.path.isdir(d):
            continue
        seen = set()
        for fname in sorted(os.listdir(d)):
            if not fname.endswith(".c"):
                continue
            src = read(os.path.join("shims", lib, fname))
            for body in CLASS_RE.findall(src):
                name = re.search(r'\.name\s*=\s*"([^"]+)"', body)
                disp = re.search(r"\.dispatch\s*=\s*(\w+)", body)
                if not name or not disp or disp.group(1) in seen:
                    continue
                seen.add(disp.group(1))
                n = len(re.findall(r"\bcase\s+(?:0x[0-9a-fA-F]+|\d+)\s*:",
                                   function_body(src, disp.group(1))))
                if n:
                    lib_ifaces[lib].append((name.group(1), n))
    return lib_ifaces


# ---- tests ---------------------------------------------------------------------------

def test_count():
    src = read("tests/CMakeLists.txt") or ""
    return len(re.findall(r"^\s*(?:s2pc_add_test\(|add_test\()", src, re.M))


# ---- history -------------------------------------------------------------------------

def history(isa_rows):
    out = subprocess.run(["git", "-C", ROOT, "log", "--reverse", "--format=%H %cs", "HEAD"],
                         capture_output=True, text=True).stdout.split("\n")
    points = []
    for line in out:
        if not line.strip():
            continue
        rev, day = line.split()
        s = svcs(read("shims/hos_cpu/svc_table.c", rev))
        svc_pct = 100 * sum(1 for x in s if x[2]) / len(s) if s else 0
        pats = shader_patterns(rev)
        isa_pct = 100 * sum(1 for e in isa_rows if covered(e, pats)) / len(isa_rows)
        points.append((day, svc_pct, isa_pct))
    return points


# ---- rendering -----------------------------------------------------------------------

def squarify(sizes, x, y, w, h):
    """Bruls-Huizing-van Wijk squarified layout; sizes sorted descending."""
    rects, total = [], float(sum(sizes))
    areas = [s * w * h / total for s in sizes] if total else []
    i = 0
    while i < len(areas):
        short = min(w, h)
        row = [areas[i]]
        i += 1

        def worst(r):
            s = sum(r)
            return max(max(short * short * a / (s * s), s * s / (short * short * a)) for a in r)

        while i < len(areas) and worst(row + [areas[i]]) <= worst(row):
            row.append(areas[i])
            i += 1
        s = sum(row)
        if w >= h:  # column on the left
            cw = s / h
            cy = y
            for a in row:
                rects.append((x, cy, cw, a / cw))
                cy += a / cw
            x, w = x + cw, w - cw
        else:  # row on top
            rh = s / w
            cx = x
            for a in row:
                rects.append((cx, y, a / rh, rh))
                cx += a / rh
            y, h = y + rh, h - rh
    return rects


def treemap(groups, width, height):
    """groups: [(title, [(label, implemented)])] -> SVG of grouped cells."""
    groups = sorted((g for g in groups if g[1]), key=lambda g: -len(g[1]))
    rects = squarify([len(g[1]) for g in groups], 0, 0, width, height)
    parts = [f'<svg viewBox="0 0 {width} {height}" class="treemap" role="img">']
    for (title, items), (x, y, w, h) in zip(groups, rects):
        parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
                     f'class="group"/>')
        head = 15 if h > 30 and w > 30 else 0
        n = len(items)
        side = max(2, int(((w - 4) * (h - head - 4) / n) ** 0.5))
        while side > 2:
            cols = max(1, int((w - 4) // side))
            if cols * int((h - head - 4) // side) >= n:
                break
            side -= 1
        cols = max(1, int((w - 4) // side))
        for k, (label, ok) in enumerate(items):
            cx, cy = x + 2 + (k % cols) * side, y + head + 2 + (k // cols) * side
            parts.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{side - 1}" '
                         f'height="{side - 1}" fill="{GREEN if ok else GRAY}">'
                         f'<title>{html.escape(title)}: {html.escape(label)}'
                         f'{"" if ok else " (missing)"}</title></rect>')
        if head:
            parts.append(f'<text x="{x + 4:.1f}" y="{y + 12:.1f}" class="glabel">'
                         f'{html.escape(title)}</text>')
    parts.append("</svg>")
    return "\n".join(parts)


def badge(label, value, pct=None):
    color = GREEN if pct is None or pct >= 90 else "#9bba22" if pct >= 70 else YELLOW \
        if pct >= 40 else RED
    return (f'<span class="badge"><span class="bl">{html.escape(label)}</span>'
            f'<span class="bv" style="background:{color}">{html.escape(value)}</span></span>')


def history_svg(points, width=900, height=220):
    if not points:
        return ""
    pad_l, pad_r, pad_t, pad_b = 44, 150, 12, 26
    pw, ph = width - pad_l - pad_r, height - pad_t - pad_b
    n = len(points)

    def px(i):
        return pad_l + (pw * i / (n - 1) if n > 1 else pw / 2)

    def py(v):
        return pad_t + ph * (1 - v / 100)

    parts = [f'<svg viewBox="0 0 {width} {height}" class="history" role="img">']
    for v in (0, 25, 50, 75, 100):
        parts.append(f'<line x1="{pad_l}" x2="{pad_l + pw}" y1="{py(v):.1f}" y2="{py(v):.1f}" '
                     f'class="gridline"/><text x="{pad_l - 6}" y="{py(v) + 4:.1f}" '
                     f'class="axis" text-anchor="end">{v}%</text>')
    for idx, (name, color) in enumerate((("Kernel calls", "#58a6ff"), ("Shaders", GREEN))):
        pts = " ".join(f"{px(i):.1f},{py(p[1 + idx]):.1f}" for i, p in enumerate(points))
        parts.append(f'<polyline points="{pts}" fill="none" stroke="{color}" stroke-width="2"/>')
        last = points[-1][1 + idx]
        parts.append(f'<circle cx="{px(n - 1):.1f}" cy="{py(last):.1f}" r="3.5" fill="{color}"/>'
                     f'<text x="{px(n - 1) + 8:.1f}" y="{py(last) + 4:.1f}" fill="{color}" '
                     f'class="legend">{name} {last:.1f}%</text>')
    first, last = points[0][0], points[-1][0]
    parts.append(f'<text x="{pad_l}" y="{height - 6}" class="axis">{first}</text>'
                 f'<text x="{pad_l + pw}" y="{height - 6}" class="axis" '
                 f'text-anchor="end">{last} ({n} commits)</text>')
    parts.append("</svg>")
    return "\n".join(parts)


CSS = """
:root { --bg:#0d1117; --panel:#161b22; --line:#30363d; --text:#e6edf3; --muted:#8b949e; }
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--text);
       font:15px/1.5 -apple-system, "Segoe UI", Helvetica, Arial, sans-serif; }
main { max-width:1100px; margin:0 auto; padding:24px 16px 48px; }
h1 { font-size:28px; margin:0 0 4px; border-bottom:1px solid var(--line); padding-bottom:8px; }
h2 { font-size:17px; margin:28px 0 8px; font-weight:600; }
.sub { color:var(--muted); margin:6px 0 16px; }
.badges { display:flex; flex-wrap:wrap; gap:8px; margin:12px 0 4px; }
.badge { display:inline-flex; font-size:13px; border-radius:3px; overflow:hidden; }
.bl { background:#555; padding:2px 8px; } .bv { padding:2px 8px; font-weight:600; }
.maps { display:grid; grid-template-columns:1fr 1fr; gap:20px; }
.maps h3 { font-size:14px; font-weight:600; margin:0 0 6px; }
@media (max-width: 760px) { .maps { grid-template-columns:1fr; } }
svg { width:100%; height:auto; display:block; }
.treemap .group { fill:#0d1117; stroke:#000; stroke-width:2; }
.treemap .glabel { fill:#fff; font-size:11px; paint-order:stroke; stroke:#000; stroke-width:3px; }
.history .gridline { stroke:var(--line); stroke-width:1; }
.history .axis { fill:var(--muted); font-size:11px; } .history .legend { font-size:12px; font-weight:600; }
table { width:100%; border-collapse:collapse; font-size:14px; }
th, td { text-align:left; padding:6px 8px; border-bottom:1px solid var(--line); }
th { color:var(--muted); font-weight:600; }
.st { display:inline-block; padding:0 8px; border-radius:10px; font-size:12px; font-weight:600; }
.legendrow { color:var(--muted); font-size:13px; margin-top:6px; }
.sq { display:inline-block; width:10px; height:10px; margin:0 4px 0 10px; vertical-align:middle; }
footer { color:var(--muted); font-size:12px; margin-top:32px; }
"""

STATUS_COLORS = {"playable": GREEN, "works": "#238636", "partial": YELLOW, "broken": RED}


def main():
    isa_rows = isa()
    pats = shader_patterns()
    isa_done = [covered(e, pats) for e in isa_rows]
    s = svcs(read("shims/hos_cpu/svc_table.c"))
    lib_ifaces = ipc()
    compat = json.load(open(os.path.join(ROOT, "docs/data/compat.json")))["entries"]
    hist = history(isa_rows)

    svc_ok = sum(1 for x in s if x[2])
    svc_pct = 100 * svc_ok / len(s)
    isa_ok = sum(isa_done)
    isa_pct = 100 * isa_ok / len(isa_rows)
    mnemonics = collections.defaultdict(list)
    for e, ok in zip(isa_rows, isa_done):
        mnemonics[e[2]].append(ok)
    mn_ok = sum(1 for v in mnemonics.values() if any(v))
    commands = sum(n for ifs in lib_ifaces.values() for _, n in ifs)
    interfaces = sum(len(ifs) for ifs in lib_ifaces.values())
    working = sum(1 for c in compat if c["status"] in ("playable", "works"))

    svc_groups = collections.defaultdict(list)
    for num, name, ok, group in s:
        svc_groups[group].append((f"{num:#04x} {name}", ok))
    isa_groups = collections.defaultdict(list)
    for (v, m, name), ok in zip(isa_rows, isa_done):
        isa_groups[isa_family(name)].append((f"{name} ({v >> 48:#06x})", ok))
    ipc_groups = [(lib.replace("hos_", ""), [(f"{iface} #{k}", True)
                                              for iface, n in ifs for k in range(n)])
                  for lib, ifs in lib_ifaces.items()]

    rows = []
    for c in compat:
        color = STATUS_COLORS.get(c["status"], GRAY)
        rows.append(f'<tr><td>{html.escape(c["name"])}</td><td>{html.escape(c["kind"])}</td>'
                    f'<td><span class="st" style="background:{color}">'
                    f'{html.escape(c["status"])}</span></td>'
                    f'<td>{html.escape(c.get("notes", ""))}</td></tr>')

    commit = subprocess.run(["git", "-C", ROOT, "log", "-1", "--format=%h %cs"],
                            capture_output=True, text=True).stdout.strip()
    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>switch2pc status</title><style>{CSS}</style></head>
<body><main>
<h1>Status</h1>
<p class="sub">switch2pc runs Nintendo Switch homebrew natively on Linux ARM64. Generated
from the sources by scripts/gen_status.py at {html.escape(commit)}.</p>
<div class="badges">
{badge("kernel calls", f"{svc_pct:.1f}%", svc_pct)}
{badge("shader instructions", f"{isa_pct:.1f}%", isa_pct)}
{badge("IPC commands", str(commands))}
{badge("homebrew working", f"{working}/{len(compat)}", 100 * working / len(compat))}
{badge("tests", str(test_count()))}
</div>
<div class="maps">
<section><h3>Kernel calls: {svc_pct:.2f}% ({svc_ok}/{len(s)})</h3>
{treemap(list(svc_groups.items()), 520, 300)}</section>
<section><h3>GPU shader instructions: {isa_pct:.2f}% ({isa_ok}/{len(isa_rows)} encodings,
{mn_ok}/{len(mnemonics)} mnemonics)</h3>
{treemap(list(isa_groups.items()), 520, 300)}</section>
</div>
<p class="legendrow"><span class="sq" style="background:{GREEN}"></span>implemented
<span class="sq" style="background:{GRAY}"></span>known, not implemented. Hover a square
for its name. Shader encodings are envytools' gm107 table; an encoding counts once the
interpreter decodes some form of it.</p>

<h2>History</h2>
{history_svg(hist)}

<h2>IPC: {commands} commands in {interfaces} service interfaces</h2>
<p class="sub">Each square is a command a service answers. HOS has thousands; the ones
here are those homebrew has asked for so far.</p>
{treemap(ipc_groups, 1060, 240)}

<h2>Homebrew</h2>
<table><thead><tr><th>Program</th><th>Kind</th><th>Status</th><th>Notes</th></tr></thead>
<tbody>
{chr(10).join(rows)}
</tbody></table>
<footer>Kernel-call names follow switchbrew; shader encodings come from envytools
(MIT). Homebrew status is kept by hand in docs/data/compat.json.</footer>
</main></body></html>
"""
    out = os.path.join(ROOT, "docs/status.html")
    with open(out, "w") as f:
        f.write(page)
    print(f"kernel calls {svc_ok}/{len(s)} ({svc_pct:.1f}%), shader encodings {isa_ok}/"
          f"{len(isa_rows)} ({isa_pct:.1f}%, {mn_ok}/{len(mnemonics)} mnemonics), "
          f"IPC {commands} commands in {interfaces} interfaces, homebrew {working}/"
          f"{len(compat)}, {len(hist)} commits of history -> docs/status.html")


if __name__ == "__main__":
    sys.exit(main())
