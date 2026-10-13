# Homebrew compatibility (nightly)

Every night a GitHub Actions workflow builds a curated list of open-source
Switch homebrew from upstream source, runs each one under switch2pc, records
how far it gets and what stopped it, and publishes the table on the
[status page](https://ctj01.github.io/switch2pc/status.html#compat). Failing
apps are expected and welcome: each blocker ("app X stops at `ssl` command 2")
is a ready-made, self-contained task, and the workflow keeps one GitHub issue
per blocker (label `compat`, plus `good first issue` when it is a single IPC
command or kernel call).

## How the pipeline works

`.github/workflows/compat.yml`, nightly at 03:00 UTC and on `workflow_dispatch`
(optionally with a space-separated list of app names):

1. **matrix**: `compat/matrix.py` turns `compat/apps.json` into a build matrix.
   Each entry gets a cache key hashed from its repository, ref, packages, build
   commands and `compat/build_app.sh`, so an unchanged app is restored from the
   Actions cache instead of being rebuilt.
2. **build**: one job per app in the `devkitpro/devkita64` container, fail-fast
   off. `compat/build_app.sh <name> <out>` clones the pinned ref (shallow, with
   submodules), installs the extra `dkp-pacman` packages, runs the entry's build
   commands from the repository root and copies the `.nro` out. A build failure
   is a result (`build.json` with the last lines of the log), never a failed job.
   The output is uploaded as an artifact named after the app.
3. **run**: on `ubuntu-24.04-arm`, native ARM64. Builds switch2pc
   (`S2PC_NO_TESTS=1 scripts/build.sh`), downloads every artifact and runs
   `compat/run.py`, which wraps `scripts/action_run.sh` for each app: SD card
   directory from `sdmc_setup`, input script from `input`, the exit request
   after `duration_ms`, frames dumped every 10th. Results go to
   `compat/results.json`, the per-level counts are appended to
   `compat/history.json` (one point per day), one frame per app becomes a PNG
   thumbnail in `docs/compat/`, and `scripts/gen_status.py` regenerates
   `docs/status.html`.
4. On `main` the results, thumbnails and page are committed (so GitHub Pages
   updates) and `compat/issues.py` creates, updates or closes the issues. On any
   other ref the results are only uploaded as the `compat-results` artifact and
   the issue script writes `compat/issues_preview.md` instead.

Every app has a timeout: hosrun is killed 60 s after the exit request was due,
and the whole run is bounded by the job's `timeout-minutes`.

## Levels

Each app lands in exactly one level, worst to best:

| Level | Meaning |
|---|---|
| `build-failed` | did not build with current devkitPro from the pinned ref |
| `crash` | the log has `fatal SIG...` (a crash in game or host code, `shims/hos_cpu/trap.c`), a fatal `svcBreak`, or hosrun exited with an error |
| `boots` | ran without crashing but presented no frame (or only black ones) |
| `renders` | presented frames with something on them |
| `runs` | renders, and exited cleanly after the exit request (or on its own exit path, e.g. Plus from the input script) |

An app that renders but ignores the exit request stays at `renders` with the
note "did not exit on the exit request". A thumbnail of the last non-black
frame is kept whatever the level: it often says more than the log (nx-hbmenu,
for instance, "runs" but shows `launchInit() failed`).

## Blockers

`compat/run.py` parses the log for the messages the shims print when they hit
something unimplemented, and keys them so the same gap in two apps is one
blocker (first in time first; all distinct ones kept):

| Log line (shim) | Key |
|---|---|
| `GetService(NAME): not registered` (`shims/hos_cpu/ipc_sm.c`) | `service:NAME` |
| `CLASS: command N not implemented` (`shims/hos_cpu/ipc.c`) | `ipc:CLASS:N` |
| `IGraphicBufferProducer transaction N not implemented` (`shims/hos_gpu/vi.c`) | `ipc:IGraphicBufferProducer:N` |
| `bsd command N is not supported` (`shims/hos_net/bsd.c`) | `ipc:bsd:u:N` |
| `unimplemented svc 0xNN NAME` (`shims/hos_cpu/svc_table.c`) | `svc:0xNN:NAME` |
| `svcGetInfo: unknown id N` (`shims/hos_cpu/svc_misc.c`) | `getinfo:N` |
| `topology N not supported`, `... shader stopped at instruction 0x...`, `texture/depth/render target format 0x.. not supported`, `2d blit: surface format ...` (`shims/hos_gpu/gpu_draw.c`, `gpu_engines.c`) | `gpu:topology:N`, `gpu:shader:0xOPCODE` (top 16 bits of the instruction), `gpu:texture-format:0x..`, `gpu:depth-format:0x..`, `gpu:rt-format:0x..`, `gpu:blit-format:0x..:0x..` |
| `subchannel N: class 0x.. not modelled`, `unknown macro/ALU operation N` | `gpu:class:0x..`, `gpu:macro-op:N`, `gpu:macro-alu:N` |
| `nvmap/nvhost-ctrl ioctl 0x.. not implemented`, `ioctl 0x.. on device N: not modelled` (`shims/hos_gpu/nvdrv.c`) | `nvdrv:nvmap:0x..`, `nvdrv:nvhost-ctrl:0x..`, `nvdrv:ioctl:0x..` |
| `setsockopt(level, option) unknown` (`shims/hos_net/bsd.c`) | `sockopt:LEVEL:OPTION` |

The rules live in `BLOCKER_RULES` in `compat/run.py`; when a shim gains a new
"not supported" message, add a rule.

## Adding an app

One object in `compat/apps.json`:

| Field | Meaning |
|---|---|
| `name` | short identifier, also the artifact and thumbnail name (`[a-z0-9-]+`) |
| `title` | what the status page shows |
| `repo` | `owner/repo` on GitHub (or a full git URL) |
| `ref` | a tag or a commit sha, never a branch: the nightly must be reproducible |
| `licence` | the upstream licence, as you verified it in the repository |
| `category` | `game`, `emulator`, `media`, `utility` or `demo` |
| `dkp_packages` | extra `dkp-pacman` packages (the container already has `switch-dev` and every `switch-portlibs`) |
| `build` | shell commands run with `bash -euo pipefail` from the repository root (`DEVKITPRO` is set, `$DEVKITPRO/tools/bin` is in `PATH`; apt works as root in the container) |
| `nro` | path of the built `.nro`, relative to the repository root |
| `duration_ms` | how long to run before the exit request |
| `input` | optional input-script lines `ms buttons_hex [lx ly rx ry]`; buttons are libnx `HidNpadButton` bits (A=0x1, B=0x2, X=0x4, Y=0x8, Plus=0x400); a state holds until the next line |
| `sdmc_setup` | optional shell run inside the empty SD card directory before the run (`$NRO` is the app's path) |
| `notes` | why it is here, what it exercises, any build workaround and why |

Before adding an entry, verify it yourself; "it is on the app store" is not
enough:

1. **Licence**: open-source, and one that allows building and running it in CI
   (MIT, ISC, BSD, GPL, MPL, Apache, public domain are fine). Write the exact
   licence you saw.
2. **Scope** (the hard rules of this repository): no title or NSP installers,
   dumpers, sigpatch or CFW tools, sysmodules, Tesla overlays, theme installers,
   cheat managers, and nothing that needs copyrighted game data or a ROM just to
   start. Emulators are fine when they reach their own UI without a ROM. Nothing
   that decrypts anything. Build from source only: no third-party binaries, no
   game data in git.
3. **It builds** with current devkitPro from the pinned ref: run it through the
   container (below). If it needs a workaround (a flag for a newer GCC, a
   missing `pkg-config` shim), put it in `build` and explain it in `notes`;
   do not patch the program's behaviour.
4. Pick a `duration_ms` long enough to get past its loading screen and a short
   `input` if it needs a button to show anything.

Rejected candidates and why are listed at the end of this file, so nobody
evaluates them twice.

## Running one app locally

Build it in the container (x86-64 or ARM64 host, Docker required):

```
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest \
    compat/build_app.sh hbmenu build/compat/hbmenu
```

Then, after `scripts/build.sh` (native ARM64, or x86-64 under qemu-user):

```
compat/run.py --nro-dir build/compat --only hbmenu --results /tmp/results.json \
    --thumbs /tmp/thumbs --markdown /tmp/summary.md
scripts/action_run.sh build/compat/hbmenu/hbmenu.nro --sdmc sd --duration 8000 --frames out
```

`run.py` prints the level and blockers; the full log is in
`build/compat/runs/<name>/hosrun.log`. `HOS_LOG_LEVEL=debug` shows the IPC
traffic around a blocker. To preview the issues the nightly would open:

```
compat/issues.py --results /tmp/results.json --dry-run /tmp/issues.md
```

## Rejected candidates

| Candidate | Why |
|---|---|
| Goldleaf, Tinfoil-likes, DBI, Awoo and other installers | title/NSP installers (rule 4); DBI is also closed source |
| nxdumptool | dumper (rule 4) |
| Atmosphère, Hekate, sigpatch tools, sys-clk, sys-ftpd, Tesla overlays, Fizeau | CFW, sysmodules and overlays (rule 4) |
| Checkpoint | save manager with a cheat manager built in (rule 4) |
| EdiZon, Breeze | save editors with cheat management (rule 4) |
| SimpleModManager | installs game mods over LayeredFS (modifies game content; rule 4) |
| sphaira | homebrew menu whose main features are installing titles and themes (rule 4) |
| sm64-port, ship of harkinian and other decomp ports | need the game's own data to start (rule 4) |
| RetroArch | GPLv3 and fine in principle, but builds for an hour; revisit with a cached core |
| NXMP | needs a patched FFmpeg (averne/nvtegra), libusbhsfs and hand-built portlibs: not reproducible in the container yet |
| Moonlight-Switch | builds through vcpkg and its own scripts; too heavy to reproduce for now |
| pplay | needs its own FFmpeg build (ffmpeg.sh) and libsmb2 from source, like NXMP |
| NoiES v1.2 | written against the pre-4.0 libnx hid API (KEY_A ...), does not build with current libnx |
| NX-Activity-Log v1.4.0 | its SimpleIniParser submodule points at a repository that no longer exists |
