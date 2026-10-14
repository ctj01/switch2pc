# Your first contribution

switch2pc runs Nintendo Switch homebrew on Linux by answering, in ordinary C,
the questions a homebrew asks the Switch's operating system. Most of the work
left is exactly that: one more question answered. You do not need to know the
whole project to help. Pick the path that matches how much time and code you
want, and ask in [Discussions](https://github.com/ctj01/switch2pc/discussions)
whenever you are stuck.

## 1. Get a working environment (5 minutes)

**GitHub Codespaces (nothing to install).** Click
[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/ctj01/switch2pc)
and wait. The container has devkitPro (to build homebrew), the ARM64 cross
compiler and qemu (to run it on an x86 machine); when it opens it builds the
test homebrew and switch2pc and runs every test (about 5 minutes the first
time). GitHub's free plan includes enough Codespaces hours a month for this.

**Your own machine with Docker.** Open the repository in VS Code and choose
"Reopen in Container" (Dev Containers extension), or build `.devcontainer/` by
hand. Same environment as Codespaces.

**By hand.** The README's Quick start; Ubuntu 24.04 or WSL2.

Check it works:

```sh
scripts/action_run.sh build/deko_examples/deko_examples.nro --frames out --duration 8000
```

`out/` now holds the frames the homebrew drew (PPM images; VS Code shows them).

## 2. No code: test and list homebrew (15-30 minutes)

Every night CI builds the apps listed in `compat/apps.json`, runs them and
publishes how far each gets on the [status page](https://ctj01.github.io/switch2pc/status.html).
Each app that stops at something missing becomes an issue someone can fix.
More apps means more of those, so adding one is a real contribution.

**Add an app.** Find an open-source Switch homebrew (GitHub search
`libnx`, or the homebrew app stores' source links). It must:

- have a licence that lets anyone build it;
- not be a piracy or system-modification tool: no title installers, dumpers,
  cheat or save injectors, CFW, sysmodules or overlays;
- start without copyrighted game data (an emulator is fine if it reaches its
  own menu without a ROM).

Copy an entry in `compat/apps.json` and change it; `compat/README.md`
explains every field. Pin `ref` to a tag or commit, never a branch. Then
test it without any hardware: push to your fork, open its **Actions** tab
(the first time, enable workflows there; forks start with them off), pick
**compat**, **Run workflow**, and type the app's `name`. The job page
shows the level it reached and the blockers. Open a pull request with what
you saw. If it does not build, say so too: `compat/README.md` keeps a list of
rejected candidates and why, so nobody tries them again.

**Report a run.** Tried an app by hand? Open an issue with the
"App compatibility report" form.

**Docs.** Something in a README or guide was unclear to you: that is a bug.
Fix the sentence and send it.

## 3. A little code: answer one more question (an evening)

Issues labelled [`good first issue`](https://github.com/ctj01/switch2pc/labels/good%20first%20issue)
are mostly single IPC commands the nightly run found missing, with the app
that needs them, the log line, and the file where the fix goes. A Switch
service is a C `switch` over command numbers; this is all of the battery
service's first commands (`shims/hos_os/psm.c`):

```c
static hos_result psm_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* GetBatteryChargePercentage -> u32 */
        return out_u32(ctx, 100);
    case 1: /* GetChargerType -> u32 */
        return out_u32(ctx, CHARGER_OFFICIAL);
    ...
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}
```

Adding a command is adding a `case`:

1. Look the command up on [switchbrew](https://switchbrew.org/wiki/Services_API):
   its number, what it takes and what it returns. libnx's headers
   (`/opt/devkitpro/libnx/include/switch/services/` in the container) show
   the exact layout the homebrew expects.
2. Read the inputs with `hos_ipc_in_read(ctx, offset, &value, size)` and
   write the outputs with `hos_ipc_out_write(ctx, &value, size)`. Return
   `HOS_RESULT_SUCCESS`, or an error result the real service would give.
3. Answer honestly: an empty list, "not found" or zero is better than
   pretending something exists. A service the game waits on (audio, video)
   must take real time, or the game's threads spin.
4. Add a check to `tests/test_ipc.c` (the neighbouring tests show how).
5. `scripts/build.sh` builds and runs every test; run the app from the issue
   with `scripts/action_run.sh` and see the log line is gone.
6. Format: `clang-format -i <files>` (CI checks it).

Then open a pull request that says which issue it fixes and what the app does
now. CONTRIBUTING.md has the full rules for services and kernel calls.

You do not need to ask for an issue first: issues are not assigned, so pick
one and open the pull request when it works. If two pull requests fix the same
thing, the first one ready to merge goes in. Opening a draft pull request early
is the way to say you are on it.

## 4. More: services, kernel calls, the GPU

- A whole missing service (`[compat] service:NAME` issues): CONTRIBUTING.md,
  "Adding a service".
- Kernel calls: grey squares under "Kernel calls" on the status page;
  CONTRIBUTING.md, "Adding an svc".
- The GPU: Maxwell shader instructions are the grey squares under "GPU shader
  instructions". `shims/hos_gpu/gpu_shader.c` interprets them,
  `tests/test_shader.c` shows how one is tested, and `docs/gpu.md` explains
  the design and what each milestone taught.

## Ground rules

- Homebrew only. No decryption, keys, NSP/XCI/NCA handling or commercial game
  data, ever; pull requests that add any are closed.
- C11, `clang-format`, no compiler warnings (they are errors).
- Small pull requests are reviewed faster. Explain what and why in the
  commit message; the history is how the next person learns.
