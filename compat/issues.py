#!/usr/bin/env python3
"""Keep one GitHub issue per blocker found by the nightly compatibility run.

    compat/issues.py --results compat/results.json --state compat/issues.json
    compat/issues.py --results compat/results.json --dry-run compat/issues_preview.md

For every distinct blocker key in results.json (service:NAME, ipc:CLASS:N,
svc:0xNN:NAME, getinfo:N, gpu:..., nvdrv:...) there is one issue titled
"[compat] <blocker>: needed by <apps>", labelled `compat` (plus `good first
issue` for a single IPC command or kernel call), whose body says what the
blocker is, which apps stop at it, the log lines, where in the code it would be
implemented and how to check a fix. Issues are found again by the hidden marker
`<!-- compat:<key> -->` in their body: an existing one is updated, not
duplicated, and closed with a comment once no app hits its blocker any more.

Blockers in system services that hold keys or manage installed content
(DENIED below: spl, set:cal, es, fsp-pr, ...) never get an issue: implementing
them is out of the project's scope. An issue is closed only after a complete
run in which every app that hit its blocker last time ran again and got past a
crash; a partial or broken run closes nothing. Issues closed as "not planned"
stay closed, and labels a maintainer added are kept.

The dry run makes no change and writes the issues it would create, update or
close to a markdown file. GITHUB_TOKEN and GITHUB_REPOSITORY (owner/repo) come
from the environment, as in Actions. Standard library only.
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARKER = "<!-- compat:{} -->"
LABEL = "compat"
EASY_LABEL = "good first issue"
MAX_CREATE = 30  # per run: a first run on a long list would otherwise hit GitHub's rate limits

# Out of scope: services that hold or use the console's keys, certificates or
# tickets, or that manage installed content and processes.
DENIED = re.compile(r"^service:(spl(:\w*)?|set:cal|es|fsp-(pr|ldr|srv-pr)|pm:\w+|ldr:\w+|ncm|lr|"
                    r"csrng:s|manu)$")

# Which shim library a missing service most likely belongs in.
SERVICE_HOME = {
    "ts": "hos_os", "pl:u": "hos_os", "pl:s": "hos_os", "ns:am2": "hos_os", "ns:ec": "hos_os",
    "pdm:qry": "hos_os", "pctl:a": "hos_os", "pctl:s": "hos_os", "pctl": "hos_os",
    "caps:a": "hos_os", "caps:c": "hos_os", "caps:su": "hos_os", "caps:sc": "hos_os",
    "ssl": "hos_net", "sfdnsres": "hos_net", "nsd:u": "hos_net", "ldn:u": "hos_net",
    "lp2p:app": "hos_net", "wlan:inf": "hos_net", "ntc": "hos_net", "nim": "hos_net",
    "audren:u": "hos_audio", "hwopus": "hos_audio", "audin:u": "hos_audio",
    "irs": "hos_input", "nfp:user": "hos_input", "hid:sys": "hos_input", "ahid:hdr": "hos_input",
    "nvdrv:a": "hos_gpu", "nvdrv:s": "hos_gpu", "vi:m": "hos_gpu", "vi:s": "hos_gpu",
    "usb:hs": "hos_os",
}


def log(msg):
    print(f"issues: {msg}", flush=True)


# ---- where in the code ----------------------------------------------------------------

def ipc_classes():
    """IPC class name -> (file, dispatch function) from the shims' hos_service_class tables."""
    out = {}
    shims = os.path.join(ROOT, "shims")
    for lib in sorted(os.listdir(shims)):
        d = os.path.join(shims, lib)
        if not os.path.isdir(d):
            continue
        for fname in sorted(os.listdir(d)):
            if not fname.endswith(".c"):
                continue
            src = open(os.path.join(d, fname), errors="replace").read()
            for body in re.findall(r"hos_service_class\s+\w+\s*=\s*\{(.*?)\};", src, re.S):
                name = re.search(r'\.name\s*=\s*"([^"]+)"', body)
                disp = re.search(r"\.dispatch\s*=\s*(\w+)", body)
                if name and disp:
                    out.setdefault(name.group(1), (f"shims/{lib}/{fname}", disp.group(1)))
    return out


def describe(key, classes):
    """(kind, what it is, where to implement it, is it a small task)."""
    parts = key.split(":")
    kind = parts[0]
    if kind == "service":
        name = key[len("service:"):]
        lib = SERVICE_HOME.get(name, "hos_os")
        what = (f"The app asks the service manager (`sm:`) for the `{name}` service and switch2pc has "
                f"no implementation of it, so `GetService` fails with \"not registered\". Most "
                f"homebrew tolerate a missing service for a while and fail later, when they use it. "
                f"What `{name}` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).")
        where = (f"A new `hos_service_class` registered under the name `{name}` from a library "
                 f"constructor with `hos_ipc_register_service` (CONTRIBUTING.md, \"Adding a "
                 f"service\"); `shims/hos_os/lm.c` is the smallest complete example and "
                 f"`shims/{lib}/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs "
                 f"the miss. Start with the commands the apps below actually call: run the app "
                 f"with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines "
                 f"that follow.")
        return kind, what, where, False
    if kind == "ipc" and len(parts) >= 3:
        cls, cmd = ":".join(parts[1:-1]), parts[-1]
        loc = classes.get(cls)
        what = (f"The app sends command {cmd} to the `{cls}` interface and switch2pc's "
                f"implementation of that interface does not handle it (`hos_ipc_unimplemented` in "
                f"`shims/hos_cpu/ipc.c` logs it and answers with `NOT_IMPLEMENTED`). The command's "
                f"name and layout are on [switchbrew](https://switchbrew.org/wiki/Services_API).")
        if loc:
            where = (f"A new `case {cmd}:` in `{loc[1]}` in `{loc[0]}`: read the request with "
                     f"`hos_ipc_in_read`, write the reply with `hos_ipc_out_write`, return a "
                     f"`hos_result`; see the neighbouring cases and the test in `tests/test_ipc.c`.")
        else:
            where = (f"The dispatch function of the `{cls}` class under `shims/` (grep for "
                     f"`.name = \"{cls}\"`): a new `case {cmd}:` next to the existing ones.")
        return kind, what, where, True
    if kind == "svc" and len(parts) >= 3:
        num, name = parts[1], parts[2]
        what = (f"The app issues the kernel call `{name}` ({num}) and switch2pc's SVC table has no "
                f"handler for it, so it returns `NOT_IMPLEMENTED`. Its semantics are on "
                f"[switchbrew](https://switchbrew.org/wiki/SVC).")
        where = (f"`shims/hos_cpu/svc_table.c` (point the `{num}` entry at a new "
                 f"`svc_<name>(hos_svc_regs *r)` in the matching `svc_*.c`), with a case in "
                 f"`tests/test_svc.c`; CONTRIBUTING.md, \"Adding an svc\".")
        return kind, what, where, True
    if kind == "getinfo":
        what = (f"The app calls `svcGetInfo` with info id {parts[1]} and switch2pc does not know "
                f"it (`svc_get_info` in `shims/hos_cpu/svc_misc.c`). The ids are listed on "
                f"[switchbrew](https://switchbrew.org/wiki/SVC#InfoType).")
        where = "A new case in `svc_get_info` in `shims/hos_cpu/svc_misc.c`, and a line in `tests/test_svc.c`."
        return kind, what, where, True
    if kind == "gpu":
        what = (f"The software GPU hit something it does not implement yet ({key}): the app's "
                f"command stream or shaders use a feature outside what `docs/gpu.md` lists as done.")
        where = ("`shims/hos_gpu/gpu_draw.c` (draw state, formats, topologies), "
                 "`shims/hos_gpu/gpu_shader*.c` (Maxwell instructions; the status page shows which "
                 "encodings are missing), `shims/hos_gpu/gpu_engines.c` (clears, copies, macros). "
                 "The deko3d examples in `examples/libnx_deko3d*` show how a feature is checked "
                 "against a reference.")
        return kind, what, where, False
    if kind == "nvdrv":
        what = (f"The app issues an `nvdrv` ioctl switch2pc does not model ({key}). The ioctls are "
                f"documented on [switchbrew](https://switchbrew.org/wiki/NV_services).")
        where = "`shims/hos_gpu/nvdrv.c`: the ioctl switch of the device named in the log."
        return kind, what, where, True
    if kind == "sockopt":
        what = f"The app sets or reads a socket option `bsd:u` does not translate ({key})."
        where = "`shims/hos_net/bsd.c`: the socket-option tables (FreeBSD numbering to Linux)."
        return kind, what, where, True
    return kind, f"switch2pc logged `{key}`, which the compatibility run treats as a blocker.", \
        "See the log line below for the shim that logged it.", False


# ---- issue text ------------------------------------------------------------------------

def collect(results):
    """key -> [(app result, log line)], first-in-time blockers first within an app.
    Out-of-scope blockers are left out."""
    by_key = {}
    for app in results["apps"]:
        for i, b in enumerate(app.get("blockers", [])):
            if DENIED.match(b["key"]):
                continue
            by_key.setdefault(b["key"], []).append((app, b["line"], i == 0))
    return by_key


def all_app_names():
    try:
        return {a["name"] for a in json.load(open(os.path.join(ROOT, "compat/apps.json")))["apps"]}
    except (OSError, ValueError, KeyError, TypeError):
        return set()


def safe_to_close(key, old_state, levels, complete):
    """A blocker that disappeared is fixed only if the run was complete and every app that
    hit it before ran again without failing to build or crashing."""
    prev = (old_state.get(key) or {}).get("apps")
    if not complete or not prev:
        return False
    return all(levels.get(name) not in (None, "build-failed", "crash") for name in prev)


def title_of(key, hits):
    names = [a["name"] for a, _, _ in hits]
    shown = ", ".join(names[:4]) + (f" and {len(names) - 4} more" if len(names) > 4 else "")
    return f"[compat] {key}: needed by {shown}"


def body_of(key, hits, results, classes):
    kind, what, where, _ = describe(key, classes)
    first = sum(1 for _, _, f in hits if f)
    # nothing that changes from night to night (dates, commits): the body is only
    # rewritten when the apps or log lines change
    lines = [f"**Blocker** `{key}`, hit by {len(hits)} app{'s' if len(hits) != 1 else ''} in the "
             f"nightly compatibility run; the first blocker in {first} of them.", "",
             "### What it is", "", what, "", "### Apps that stop at it", ""]
    for app, line, is_first in hits:
        repo = app.get("repo", "")
        link = f"https://github.com/{repo}/tree/{app.get('ref', '')}" if repo and "://" not in repo else repo
        lines.append(f"- **{app.get('title') or app['name']}** ([{repo}@{app.get('ref', '')}]({link})), "
                     f"level `{app['level']}`, {app.get('frames', 0)} frames"
                     f"{', first blocker' if is_first else ''}")
    lines += ["", "### Log", "", "```"]
    for app, line, _ in hits[:8]:
        lines.append(f"{app['name']}: {line}")
    lines += ["```", "", "### Where it goes", "", where, "", "### How to check a fix", "",
              f"Build the app with `compat/build_app.sh <name> build/compat/<name>` in the "
              f"`devkitpro/devkita64` container, then run it the way the nightly does:", "", "```",
              f"compat/run.py --nro-dir build/compat --only {hits[0][0]['name']} --results /tmp/r.json",
              f"scripts/action_run.sh build/compat/{hits[0][0]['name']}/{hits[0][0]['name']}.nro --duration 8000 --frames out",
              "```", "",
              "The log line above should be gone and the app's level should not drop; see "
              "compat/README.md for the levels. This issue is maintained by the nightly run "
              "(compat/issues.py): it is updated while apps hit the blocker and closed when none does.",
              "", MARKER.format(key)]
    return "\n".join(lines)


def labels_of(key, classes):
    _, _, _, easy = describe(key, classes)
    return [LABEL, EASY_LABEL] if easy else [LABEL]


# ---- GitHub --------------------------------------------------------------------------------

class GitHub:
    def __init__(self, repo, token):
        self.repo, self.token = repo, token
        self.base = f"https://api.github.com/repos/{repo}"

    def call(self, method, path, data=None, params=""):
        req = urllib.request.Request(self.base + path + params, method=method,
                                     data=json.dumps(data).encode() if data is not None else None)
        req.add_header("Authorization", f"Bearer {self.token}")
        req.add_header("Accept", "application/vnd.github+json")
        req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return json.loads(resp.read() or b"null")
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"{method} {path}: {e.code} {e.read().decode(errors='replace')[:300]}")

    def issues_with_marker(self):
        """key -> issue, for every issue carrying a compat marker (open or closed)."""
        found = {}
        page = 1
        while True:
            batch = self.call("GET", "/issues", params=f"?state=all&labels={LABEL}&per_page=100&page={page}")
            for issue in batch:
                if "pull_request" in issue:
                    continue
                m = re.search(r"<!-- compat:(\S+) -->", issue.get("body") or "")
                if m and m.group(1) not in found:
                    found[m.group(1)] = issue
            if len(batch) < 100:
                return found
            page += 1

    def ensure_labels(self):
        for name, color, desc in ((LABEL, "0e8a16", "found by the nightly homebrew compatibility run"),
                                  (EASY_LABEL, "7057ff", "")):
            try:
                self.call("GET", f"/labels/{urllib.request.quote(name)}")
            except RuntimeError:
                try:
                    self.call("POST", "/labels", {"name": name, "color": color, "description": desc})
                except RuntimeError as e:
                    log(f"cannot create label {name}: {e}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", default=os.path.join(ROOT, "compat/results.json"))
    ap.add_argument("--state", default=None, help="write key -> issue number/url here (for the status page)")
    ap.add_argument("--dry-run", metavar="PREVIEW.md", help="change nothing; write what would happen here")
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", "ctj01/switch2pc"))
    args = ap.parse_args()

    results = json.load(open(args.results))
    classes = ipc_classes()
    by_key = collect(results)
    old_state = {}
    if args.state and os.path.exists(args.state):
        try:
            old_state = json.load(open(args.state))
        except ValueError:
            old_state = {}
    levels = {a["name"]: a.get("level") for a in results["apps"]}
    expected = all_app_names()
    complete = bool(expected) and expected <= set(levels)
    if not complete:
        log(f"partial run ({len(levels)} of {len(expected)} apps): nothing will be closed")
    ranked = sorted(by_key.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    gh = GitHub(args.repo, token) if token else None
    existing = {}
    if gh:
        try:
            existing = gh.issues_with_marker()
        except RuntimeError as e:
            if not args.dry_run:
                raise
            log(f"cannot list existing issues ({e}); the preview assumes there are none")
    log(f"{len(ranked)} blockers, {len(existing)} existing issues with a marker")

    plan = []  # (action, key, title, body, labels, issue or None)
    for key, hits in ranked:
        title, body, labels = title_of(key, hits), body_of(key, hits, results, classes), labels_of(key, classes)
        issue = existing.get(key)
        if not issue:
            plan.append(("create", key, title, body, labels, None))
        elif issue["state"] != "open" and issue.get("state_reason") == "not_planned":
            plan.append(("keep", key, title, body, labels, issue))  # a maintainer decided
        elif issue["state"] != "open":
            plan.append(("reopen", key, title, body, labels, issue))
        elif issue["title"] != title or (issue.get("body") or "").strip() != body.strip():
            plan.append(("update", key, title, body, labels, issue))
        else:
            plan.append(("keep", key, title, body, labels, issue))
    for key, issue in existing.items():
        if key not in by_key and issue["state"] == "open":
            if DENIED.match(key):
                continue
            if safe_to_close(key, old_state, levels, complete):
                plan.append(("close", key, issue["title"], "", [], issue))
            else:
                plan.append(("keep", key, issue["title"], "", [], issue))

    if args.dry_run:
        out = [f"# Compatibility issues: dry run", "",
               f"Results of {results.get('generated_at', '')} at switch2pc "
               f"{results.get('switch2pc_commit', '')}: {len(ranked)} distinct blockers.", ""]
        for action, key, title, body, labels, issue in plan:
            ref = f" (#{issue['number']})" if issue else ""
            out.append(f"## {action}: {title}{ref}")
            out.append("")
            if action in ("create", "update", "reopen"):
                out.append(f"labels: {', '.join(labels)}")
                out.append("")
                out.append(body.replace(MARKER.format(key), f"`{MARKER.format(key)}`"))
            elif action == "close":
                out.append("No app hits this blocker any more; it would be closed with a comment.")
            out.append("")
        with open(args.dry_run, "w") as f:
            f.write("\n".join(out))
        counts = {a: sum(1 for p in plan if p[0] == a) for a in ("create", "update", "reopen", "keep", "close")}
        log(" ".join(f"{k}={v}" for k, v in counts.items()) + f" -> {args.dry_run}")
        return 0

    if not gh:
        sys.exit("GITHUB_TOKEN is not set (use --dry-run to preview)")
    gh.ensure_labels()
    state = {}
    created = 0
    for action, key, title, body, labels, issue in plan:
        if action == "create":
            if created >= MAX_CREATE:
                log(f"create limit reached; {key} waits for the next run")
                continue
            issue = gh.call("POST", "/issues", {"title": title, "body": body, "labels": labels})
            created += 1
            time.sleep(2)  # GitHub's secondary rate limit on content creation
            log(f"created #{issue['number']} {title}")
        elif action in ("update", "reopen"):
            kept = {lb["name"] for lb in issue.get("labels", [])}  # keep what maintainers added
            issue = gh.call("PATCH", f"/issues/{issue['number']}",
                            {"title": title, "body": body, "labels": sorted(kept | set(labels)),
                             "state": "open"})
            time.sleep(1)
            log(f"{action}d #{issue['number']} {title}")
        elif action == "close":
            gh.call("POST", f"/issues/{issue['number']}/comments",
                    {"body": f"No app in the nightly compatibility run of {results.get('generated_at', '')} "
                             f"(switch2pc {results.get('switch2pc_commit', '')}) hits this blocker any "
                             f"more, so it is closed. It will be reopened if one does again."})
            issue = gh.call("PATCH", f"/issues/{issue['number']}", {"state": "closed", "state_reason": "completed"})
            log(f"closed #{issue['number']} {title}")
        if key in by_key:
            state[key] = {"number": issue["number"], "url": issue["html_url"], "state": issue["state"],
                          "apps": sorted({a["name"] for a, _, _ in by_key[key]})}
        elif action == "keep" and key in old_state:
            state[key] = old_state[key]  # not seen this run and not safely closable: remember it
    if args.state:
        with open(args.state, "w") as f:
            json.dump(state, f, indent=1, sort_keys=True)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
