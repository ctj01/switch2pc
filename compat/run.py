#!/usr/bin/env python3
"""Run the homebrew of compat/apps.json under switch2pc and classify each one.

    compat/run.py --nro-dir build/compat --out build/compat/runs \
                  [--only NAME ...] [--results compat/results.json] \
                  [--history compat/history.json] [--thumbs docs/compat]

For every app: the .nro built by compat/build_app.sh (<nro-dir>/<name>/<name>.nro,
or <nro-dir>/<name>.nro, next to its build.json) is run through
scripts/action_run.sh with its input script, SD card setup and duration, frames
dumped sparsely. The log is then classified into one level, worst to best:

    build-failed  the .nro could not be built (build.json says why)
    crash         fatal signal, fatal svcBreak, or hosrun ended with an error
    boots         ran without crashing but presented no frame
    renders       presented frames with non-black pixels
    runs          renders, and exited cleanly when asked to

and mined for blockers: the services, IPC commands, kernel calls and GPU
features switch2pc does not have yet, keyed so the same gap in two apps is one
blocker (see BLOCKER_RULES). Standard library only.
"""
import argparse
import datetime
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = ["build-failed", "crash", "boots", "renders", "runs"]

# (regex on one log line, key template). The first match in time matters most;
# every distinct key is kept. Messages come from the shims:
#   shims/hos_cpu/ipc_sm.c, ipc.c, svc_table.c, svc_misc.c
#   shims/hos_gpu/gpu_draw.c, gpu_engines.c, nvdrv.c, vi.c, macro.c
#   shims/hos_net/bsd.c
# switch2pc's own warnings and errors; the game's text is always logged under "game"
SHIM_LINE = re.compile(r"^\[s2pc (?!game\s*\])[\w ]+\] [WE]: ")
BLOCKER_RULES = [
    (r"GetService\(([\w:.-]{1,16})\): not registered", "service:{0}"),
    (r"\] W: (\S+): command (\d+) not implemented", "ipc:{0}:{1}"),
    (r"IGraphicBufferProducer transaction (\d+) not implemented", "ipc:IGraphicBufferProducer:{0}"),
    (r"bsd command (\d+) is not supported", "ipc:bsd:u:{0}"),
    (r"unknown control command (\d+)", "ipc:control:{0}"),
    (r"(?:set|get)sockopt\(level (0x[0-9a-f]+), option (0x[0-9a-f]+)\) unknown",
     "sockopt:{0}:{1}"),
    (r"unimplemented svc (0x[0-9a-f]+) (\S+)", "svc:{0}:{1}"),
    (r"svcGetInfo: unknown id (\d+)", "getinfo:{0}"),
    (r"topology (\d+) not supported", "gpu:topology:{0}"),
    (r"(fragment|vertex|compute) shader stopped at instruction (0x[0-9a-f]+)",
     "gpu:shader:{opcode}"),
    (r"texture format (0x[0-9a-f]+) layout (\d+) not supported", "gpu:texture-format:{0}"),
    (r"depth format (0x[0-9a-f]+) not supported", "gpu:depth-format:{0}"),
    (r"render target \d+ format (0x[0-9a-f]+) .*not supported", "gpu:rt-format:{0}"),
    (r"2d blit: surface format (0x[0-9a-f]+) -> (0x[0-9a-f]+) not supported",
     "gpu:blit-format:{0}:{1}"),
    (r"subchannel \d+: class (0x[0-9a-f]+) not modelled", "gpu:class:{0}"),
    (r"unknown macro operation (\d+)", "gpu:macro-op:{0}"),
    (r"unknown ALU operation (\d+)", "gpu:macro-alu:{0}"),
    (r"nvmap ioctl (0x[0-9a-f]+) not implemented", "nvdrv:nvmap:{0}"),
    (r"nvhost-ctrl ioctl (0x[0-9a-f]+) not implemented", "nvdrv:nvhost-ctrl:{0}"),
    (r"ioctl (0x[0-9a-f]+) on device (\d+): not modelled", "nvdrv:ioctl:{0}"),
]
CRASH_RULES = [
    r"\] E: fatal SIG\w+",
    r"\] E: svcBreak ",
]
# Log lines worth keeping as the excerpt: warnings, errors, the game's own output
# and the run's milestones, but not the per-frame and per-submit chatter.
NOISE = re.compile(r"\] [IDT]: (frame \d+:|submit \d+ on channel|buttons )")


def log(msg):
    print(f"compat: {msg}", flush=True)


def blockers_of(lines):
    found = []
    for line in lines:
        if not SHIM_LINE.match(line):
            continue  # only switch2pc's warnings and errors, never the guest's own output
        for pattern, key in BLOCKER_RULES:
            m = re.search(pattern, line)
            if not m:
                continue
            groups = [g.lower() if g and g.startswith("0x") else g for g in m.groups()]
            if "{opcode}" in key:
                # the top 16 bits are the opcode, what the interpreter decodes on
                word = int(groups[1], 16)
                key = key.replace("{opcode}", f"{word >> 48:#06x}")
            k = key.format(*groups)
            if k not in [b["key"] for b in found]:
                found.append({"key": k, "line": line.strip()[:300]})
            break
    return found


def crash_of(lines, status):
    for line in lines:
        if not SHIM_LINE.match(line):
            continue
        for pattern in CRASH_RULES:
            if re.search(pattern, line):
                return line.split("] E: ", 1)[-1].strip()[:200]
    if status == -1:
        return None  # the wrapper itself timed out: reported as a note, not a crash
    if status not in (0, 124):
        return f"hosrun exited with status {status}"
    return None


def excerpt(lines, n=30):
    kept = [l.rstrip() for l in lines if not NOISE.search(l)]
    return kept[-n:]


# ---- frames ----------------------------------------------------------------------------

def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, data[m.end():m.end() + w * h * 3]


def non_black(frame, step=7):
    w, h, px = frame
    return any(px[i] > 8 for i in range(0, len(px), 3 * step))


def thumbnail(frame, out, width=192):
    """Nearest-neighbour downscale to a PNG, with zlib only."""
    w, h, px = frame
    tw = min(width, w)
    th = max(1, round(h * tw / w))
    raw = bytearray()
    for y in range(th):
        sy = min(h - 1, y * h // th)
        raw.append(0)
        row = memoryview(px)[sy * w * 3:(sy + 1) * w * 3]
        for x in range(tw):
            sx = min(w - 1, x * w // tw)
            raw += row[sx * 3:sx * 3 + 3]

    def chunk(kind, body):
        c = struct.pack(">I", len(body)) + kind + body
        return c + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", tw, th, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    with open(out, "wb") as f:
        f.write(png)


def pick_frame(frames_dir):
    """The last dumped frame with something on it, else the last one."""
    names = sorted(n for n in os.listdir(frames_dir) if n.endswith(".ppm")) \
        if os.path.isdir(frames_dir) else []
    last = None
    for name in reversed(names):
        frame = read_ppm(os.path.join(frames_dir, name))
        if not frame:
            continue
        if last is None:
            last = (name, frame, False)
        if non_black(frame):
            return name, frame, True
    return last


# ---- one app -----------------------------------------------------------------------------

def find_nro(nro_dir, name):
    for cand in (os.path.join(nro_dir, name, f"{name}.nro"), os.path.join(nro_dir, f"{name}.nro")):
        if os.path.isfile(cand):
            return cand
    return None


def find_build(nro_dir, name):
    for cand in (os.path.join(nro_dir, name, "build.json"), os.path.join(nro_dir, f"{name}.build.json"),
                 os.path.join(nro_dir, "build.json")):
        if os.path.isfile(cand):
            try:
                b = json.load(open(cand))
            except ValueError:
                continue
            if b.get("name") == name:
                return b
    return None


def run_app(app, args):
    name = app["name"]
    result = {k: app.get(k) for k in ("name", "title", "repo", "ref", "licence", "category", "notes")}
    result.update({"level": "build-failed", "frames": 0, "seconds": 0, "exit_status": None,
                   "blockers": [], "crash": None, "log_tail": [], "thumbnail": None,
                   "build_seconds": None})
    build = find_build(args.nro_dir, name)
    nro = find_nro(args.nro_dir, name)
    if build:
        result["build_seconds"] = build.get("seconds")
    if not nro or (build and not build.get("ok", True)):
        result["log_tail"] = (build or {}).get("log_tail") or ["no .nro and no build log"]
        log(f"{name}: build-failed")
        return result

    work = os.path.join(args.out, name)
    shutil.rmtree(work, ignore_errors=True)
    sdmc = os.path.join(work, "sdmc")
    frames = os.path.join(work, "frames")
    os.makedirs(sdmc)
    os.makedirs(frames)
    packed = os.path.join(os.path.dirname(nro), "sdmc")
    if os.path.isdir(packed):  # files build_app.sh took from the repository ("sdmc" entries)
        shutil.copytree(packed, sdmc, dirs_exist_ok=True)
    if app.get("sdmc_setup"):
        setup = subprocess.run(["bash", "-euo", "pipefail", "-c", app["sdmc_setup"]], cwd=sdmc,
                               env=dict(os.environ, NRO=os.path.abspath(nro)),
                               capture_output=True, text=True)
        if setup.returncode != 0:  # one broken entry must not stop the whole run
            result["log_tail"] = ["sdmc_setup failed:"] + (setup.stderr or "").splitlines()[-10:]
            result["note"] = "sdmc_setup failed"
            log(f"{name}: sdmc_setup failed; skipped")
            return result
    input_path = os.path.join(work, "input.txt")
    with open(input_path, "w") as f:
        f.write("\n".join(app.get("input", [])) + "\n")
    logfile = os.path.join(work, "hosrun.log")
    duration = int(app.get("duration_ms", 10000))
    cmd = [os.path.join(ROOT, "scripts/action_run.sh"), nro, "--sdmc", sdmc, "--input", input_path,
           "--duration", str(duration), "--frames", frames, "--every", str(args.every),
           "--log", logfile]
    env = dict(os.environ)
    env.setdefault("HOS_LOG_LEVEL", "info")
    env.setdefault("HOS_INPUT_LOG", "0")
    log(f"{name}: running for {duration} ms")
    t0 = time.time()
    # its own process group, so a timeout kills hosrun and everything it started
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                            start_new_session=True)
    try:
        out, _ = proc.communicate(timeout=duration / 1000 + args.grace)
        wrapper_out = out.decode(errors="replace")
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        out, _ = proc.communicate()
        wrapper_out = (out or b"").decode(errors="replace") + "\naction_run.sh itself timed out"
        result["note"] = "the run itself timed out"
    result["seconds"] = round(time.time() - t0, 1)

    lines = open(logfile, errors="replace").read().splitlines() if os.path.isfile(logfile) else []
    m = re.search(r"exited with (\d+) after", wrapper_out or "")
    status = int(m.group(1)) if m else -1
    result["exit_status"] = status
    presented = [int(x) for x in re.findall(r"frames presented: (\d+)", "\n".join(lines))]
    shown = [int(x) for x in re.findall(r"\] I: frame (\d+):", "\n".join(lines))]
    frames_n = max(presented + shown + [0])
    result["frames"] = frames_n
    result["blockers"] = blockers_of(lines)
    result["crash"] = crash_of(lines, status)
    result["log_tail"] = excerpt(lines)

    picked = pick_frame(frames)
    drawn = bool(re.search(r"\] I: frame \d+: .* [1-9]\d* non-black pixels", "\n".join(lines)))
    if picked:
        pname, frame, has_pixels = picked
        drawn = drawn or has_pixels
        os.makedirs(args.thumbs, exist_ok=True)
        thumb = os.path.join(args.thumbs, f"{name}.png")
        thumbnail(frame, thumb)
        result["thumbnail"] = os.path.relpath(thumb, ROOT)
        result["frame"] = pname
    shutil.rmtree(frames, ignore_errors=True)

    if result["crash"]:
        level = "crash"
    elif frames_n == 0 or not drawn:
        level = "boots"
        if frames_n and not drawn:
            result["note"] = "every presented frame was black"
    elif status == 0:
        level = "runs"
    else:
        level = "renders"
        if status == 124:
            result["note"] = "did not exit on the exit request"
    result["level"] = level
    log(f"{name}: {level}, {frames_n} frames, {result['seconds']} s, exit {status}, "
        f"blockers {[b['key'] for b in result['blockers']]}")
    return result


# ---- results and history -----------------------------------------------------------------

def counts_of(results):
    return {lvl: sum(1 for r in results if r["level"] == lvl) for lvl in LEVELS}


def write_history(path, results, now, commit):
    hist = []
    if os.path.isfile(path):
        hist = json.load(open(path))
    day = now[:10]
    entry = {"date": day, "generated_at": now, "switch2pc_commit": commit,
             "counts": counts_of(results), "total": len(results)}
    # one point per day: a re-run replaces the day's entry rather than appending
    hist = [h for h in hist if h.get("date") != day] + [entry]
    with open(path, "w") as f:
        json.dump(hist, f, indent=1)
        f.write("\n")


def markdown(results, now, commit):
    lines = [f"| App | Category | Level | Frames | Blockers |", "|---|---|---|---|---|"]
    for r in results:
        keys = ", ".join(f"`{b['key']}`" for b in r["blockers"][:3])
        if len(r["blockers"]) > 3:
            keys += f" +{len(r['blockers']) - 3}"
        if r.get("crash"):
            keys = f"{r['crash']}" + (f"; {keys}" if keys else "")
        if r.get("note"):
            keys += f" ({r['note']})" if keys else r["note"]
        lines.append(f"| {r.get('title') or r['name']} | {r.get('category', '')} | **{r['level']}** | "
                     f"{r['frames']} | {keys} |")
    counts = ", ".join(f"{k} {v}" for k, v in counts_of(results).items())
    return (f"**Homebrew compatibility** at {commit}, {now}: {counts}\n\n" + "\n".join(lines)
            + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apps", default=os.path.join(ROOT, "compat/apps.json"))
    ap.add_argument("--nro-dir", required=True, help="where build_app.sh put the .nro files")
    ap.add_argument("--out", default=os.path.join(ROOT, "build/compat/runs"), help="work directory")
    ap.add_argument("--only", nargs="*", help="run these apps only")
    ap.add_argument("--results", default=os.path.join(ROOT, "compat/results.json"))
    ap.add_argument("--history", default=None, help="append the level counts to this file")
    ap.add_argument("--thumbs", default=os.path.join(ROOT, "docs/compat"))
    ap.add_argument("--markdown", default=None, help="also write the table as markdown here")
    ap.add_argument("--every", type=int, default=10, help="dump every n-th frame")
    ap.add_argument("--grace", type=int, default=120,
                    help="seconds past the duration before a run is abandoned")
    args = ap.parse_args()

    apps = json.load(open(args.apps))["apps"]
    if args.only:
        apps = [a for a in apps if a["name"] in args.only]
    os.makedirs(args.out, exist_ok=True)
    results = [run_app(a, args) for a in apps]
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
                            capture_output=True, text=True).stdout.strip()
    with open(args.results, "w") as f:
        json.dump({"generated_at": now, "switch2pc_commit": commit, "counts": counts_of(results),
                   "apps": results}, f, indent=1)
        f.write("\n")
    if args.history:
        write_history(args.history, results, now, commit)
    if args.markdown:
        with open(args.markdown, "w") as f:
            f.write(markdown(results, now, commit))
    log(" ".join(f"{k}={v}" for k, v in counts_of(results).items()) + f" -> {args.results}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
