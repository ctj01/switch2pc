#!/usr/bin/env python3
"""Print the build matrix for .github/workflows/compat.yml as JSON.

    compat/matrix.py [name ...]      # all apps, or only the named ones

One element per app: its name plus a cache key that changes whenever anything
that affects the build does (repository, ref, packages, build commands, the
build script itself), so an unchanged app is served from the Actions cache
instead of being rebuilt every night.
"""
import hashlib
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(argv):
    apps = json.load(open(os.path.join(ROOT, "compat/apps.json")))["apps"]
    script = open(os.path.join(ROOT, "compat/build_app.sh"), "rb").read()
    only = set(argv)
    out = []
    for a in apps:
        if only and a["name"] not in only:
            continue
        spec = json.dumps([a["repo"], a["ref"], a.get("dkp_packages", []), a["build"], a["nro"]])
        h = hashlib.sha256(spec.encode() + script).hexdigest()[:16]
        out.append({"name": a["name"], "key": f"{a['name']}-{h}"})
    unknown = only - {a["name"] for a in apps}
    if unknown:
        sys.exit(f"not in compat/apps.json: {', '.join(sorted(unknown))}")
    print(json.dumps(out))


if __name__ == "__main__":
    main(sys.argv[1:])
