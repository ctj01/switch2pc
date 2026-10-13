#!/usr/bin/env python3
"""Take a nightly run's results into the repository, as data.

    compat/publish.py --from <directory the compat-results artifact was downloaded to>

The run job executes third-party homebrew and has no write access; this step,
which does, trusts nothing it produced: results.json and history.json must parse
and have the expected shape, results may only name apps in compat/apps.json,
thumbnails must be small PNG files named after an app, and the status page is
not copied (the workflow regenerates it from the data with the repository's own
scripts/gen_status.py). Anything else in the artifact is ignored. Standard
library only.
"""
import argparse
import json
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG = b"\x89PNG\r\n\x1a\n"
MAX_THUMB = 256 * 1024
NAME = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
LEVELS = {"build-failed", "crash", "boots", "renders", "runs"}


def fail(msg):
    sys.exit(f"publish: {msg}")


def check_results(results, names):
    if not isinstance(results, dict) or not isinstance(results.get("apps"), list):
        fail("results.json: no app list")
    for app in results["apps"]:
        if not isinstance(app, dict) or app.get("name") not in names:
            fail(f"results.json: unknown app {app.get('name') if isinstance(app, dict) else app!r}")
        if app.get("level") not in LEVELS:
            fail(f"results.json: {app['name']}: bad level {app.get('level')!r}")
        thumb = app.get("thumbnail")
        if thumb is not None and thumb != f"docs/compat/{app['name']}.png":
            fail(f"results.json: {app['name']}: unexpected thumbnail path {thumb!r}")
        for b in app.get("blockers", []):
            if not isinstance(b, dict) or not isinstance(b.get("key"), str) or len(b["key"]) > 120:
                fail(f"results.json: {app['name']}: malformed blocker")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="src", required=True)
    args = ap.parse_args()

    names = {a["name"] for a in json.load(open(os.path.join(ROOT, "compat/apps.json")))["apps"]}
    src_results = os.path.join(args.src, "compat/results.json")
    src_history = os.path.join(args.src, "compat/history.json")
    try:
        results = json.load(open(src_results))
        history = json.load(open(src_history))
    except (OSError, ValueError) as e:
        fail(f"cannot read the run's results: {e}")
    check_results(results, names)
    if not isinstance(history, (list, dict)):
        fail("history.json: unexpected shape")

    with open(os.path.join(ROOT, "compat/results.json"), "w") as f:
        json.dump(results, f, indent=1)
        f.write("\n")
    with open(os.path.join(ROOT, "compat/history.json"), "w") as f:
        json.dump(history, f, indent=1)
        f.write("\n")

    dst = os.path.join(ROOT, "docs/compat")
    shutil.rmtree(dst, ignore_errors=True)
    os.makedirs(dst)
    taken = 0
    thumbs = os.path.join(args.src, "docs/compat")
    for fname in sorted(os.listdir(thumbs)) if os.path.isdir(thumbs) else []:
        path = os.path.join(thumbs, fname)
        stem = fname[:-4] if fname.endswith(".png") else ""
        if not stem or not NAME.match(stem) or stem not in names or os.path.islink(path):
            continue
        if not os.path.isfile(path) or os.path.getsize(path) > MAX_THUMB:
            continue
        with open(path, "rb") as f:
            data = f.read()
        if not data.startswith(PNG):
            continue
        with open(os.path.join(dst, fname), "wb") as f:
            f.write(data)
        taken += 1
    print(f"publish: {len(results['apps'])} apps, {taken} thumbnails")
    return 0


if __name__ == "__main__":
    sys.exit(main())
