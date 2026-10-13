## What and why

<!-- What does this change, and why? Link the issue it fixes ("Fixes #123"). -->

## How it was checked

<!-- e.g. scripts/build.sh passes; scripts/action_run.sh <app> no longer logs "<blocker>";
     a new test in tests/test_ipc.c. For compat/apps.json: the compat run in your fork. -->

- [ ] `scripts/build.sh` passes (CI runs it too)
- [ ] `clang-format` applied to the C files I touched
- [ ] Homebrew only: nothing here decrypts, handles keys or commercial game data
