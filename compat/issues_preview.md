# Compatibility issues: dry run

Results of 2026-10-13T02:30:28Z at switch2pc 2bb3907: 24 distinct blockers.

## create: [compat] service:audren:u: needed by hb-appstore, open-supaplex, audren-simple, sdl2-demo

labels: compat

**Blocker** `service:audren:u`, hit by 4 apps in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 3 of them.

### What it is

The app asks the service manager (`sm:`) for the `audren:u` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `audren:u` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **hb-appstore** ([vgmoose/hb-appstore@v2.3.2](https://github.com/vgmoose/hb-appstore/tree/v2.3.2)), level `renders`, 1 frames
- **OpenSupaplex** ([sergiou87/open-supaplex@v7.2.2](https://github.com/sergiou87/open-supaplex/tree/v7.2.2)), level `boots`, 1 frames, first blocker
- **switch-examples: audio/audren-simple** ([switchbrew/switch-examples@v20260201](https://github.com/switchbrew/switch-examples/tree/v20260201)), level `runs`, 269 frames, first blocker
- **switch-examples: graphics/sdl2/sdl2-demo** ([switchbrew/switch-examples@v20260201](https://github.com/switchbrew/switch-examples/tree/v20260201)), level `runs`, 73 frames, first blocker

### Log

```
hb-appstore: [s2pc sm   ] W: GetService(audren:u): not registered
open-supaplex: [s2pc sm   ] W: GetService(audren:u): not registered
audren-simple: [s2pc sm   ] W: GetService(audren:u): not registered
sdl2-demo: [s2pc sm   ] W: GetService(audren:u): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `audren:u` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_audio/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only hb-appstore --results /tmp/r.json
scripts/action_run.sh build/compat/hb-appstore/hb-appstore.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:audren:u -->`

## create: [compat] ipc:applet:ILibraryAppletCreator:0: needed by account, software-keyboard

labels: compat, good first issue

**Blocker** `ipc:applet:ILibraryAppletCreator:0`, hit by 2 apps in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 2 of them.

### What it is

The app sends command 0 to the `applet:ILibraryAppletCreator` interface and switch2pc's implementation of that interface does not handle it (`hos_ipc_unimplemented` in `shims/hos_cpu/ipc.c` logs it and answers with `NOT_IMPLEMENTED`). The command's name and layout are on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **switch-examples: account** ([switchbrew/switch-examples@v20260201](https://github.com/switchbrew/switch-examples/tree/v20260201)), level `runs`, 229 frames, first blocker
- **switch-examples: applet/libapplets/software-keyboard** ([switchbrew/switch-examples@v20260201](https://github.com/switchbrew/switch-examples/tree/v20260201)), level `runs`, 293 frames, first blocker

### Log

```
account: [s2pc ipc  ] W: applet:ILibraryAppletCreator: command 0 not implemented (in 8 bytes, 0 buffers)
software-keyboard: [s2pc ipc  ] W: applet:ILibraryAppletCreator: command 0 not implemented (in 8 bytes, 0 buffers)
```

### Where it goes

A new `case 0:` in `stub_dispatch` in `shims/hos_os/applet.c`: read the request with `hos_ipc_in_read`, write the reply with `hos_ipc_out_write`, return a `hos_result`; see the neighbouring cases and the test in `tests/test_ipc.c`.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only account --results /tmp/r.json
scripts/action_run.sh build/compat/account/account.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:ipc:applet:ILibraryAppletCreator:0 -->`

## create: [compat] ipc:sfdnsres:6: needed by curl-example, hb-appstore

labels: compat, good first issue

**Blocker** `ipc:sfdnsres:6`, hit by 2 apps in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app sends command 6 to the `sfdnsres` interface and switch2pc's implementation of that interface does not handle it (`hos_ipc_unimplemented` in `shims/hos_cpu/ipc.c` logs it and answers with `NOT_IMPLEMENTED`). The command's name and layout are on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **switch-examples: network/curl** ([switchbrew/switch-examples@v20260201](https://github.com/switchbrew/switch-examples/tree/v20260201)), level `renders`, 1 frames
- **hb-appstore** ([vgmoose/hb-appstore@v2.3.2](https://github.com/vgmoose/hb-appstore/tree/v2.3.2)), level `renders`, 1 frames

### Log

```
curl-example: [s2pc ipc  ] W: sfdnsres: command 6 not implemented (in 16 bytes, 4 buffers)
hb-appstore: [s2pc ipc  ] W: sfdnsres: command 6 not implemented (in 16 bytes, 4 buffers)
```

### Where it goes

A new `case 6:` in `sfdnsres_dispatch` in `shims/hos_net/bsd.c`: read the request with `hos_ipc_in_read`, write the reply with `hos_ipc_out_write`, return a `hos_result`; see the neighbouring cases and the test in `tests/test_ipc.c`.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only curl-example --results /tmp/r.json
scripts/action_run.sh build/compat/curl-example/curl-example.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:ipc:sfdnsres:6 -->`

## create: [compat] service:clkrst: needed by switchident, noods

labels: compat

**Blocker** `service:clkrst`, hit by 2 apps in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 1 of them.

### What it is

The app asks the service manager (`sm:`) for the `clkrst` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `clkrst` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames
- **NooDS** ([Hydr8gon/NooDS@4cb1439c330248b26cdf5f8dbcfa4e5ac53dcda0](https://github.com/Hydr8gon/NooDS/tree/4cb1439c330248b26cdf5f8dbcfa4e5ac53dcda0)), level `renders`, 400 frames, first blocker

### Log

```
switchident: [s2pc sm   ] W: GetService(clkrst): not registered
noods: [s2pc sm   ] W: GetService(clkrst): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `clkrst` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:clkrst -->`

## create: [compat] service:pl:u: needed by nx-shell, shared-font

labels: compat

**Blocker** `service:pl:u`, hit by 2 apps in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 1 of them.

### What it is

The app asks the service manager (`sm:`) for the `pl:u` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `pl:u` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **NX-Shell** ([joel16/NX-Shell@4.01](https://github.com/joel16/NX-Shell/tree/4.01)), level `boots`, 0 frames
- **switch-examples: graphics/shared_font** ([switchbrew/switch-examples@v20260201](https://github.com/switchbrew/switch-examples/tree/v20260201)), level `crash`, 0 frames, first blocker

### Log

```
nx-shell: [s2pc sm   ] W: GetService(pl:u): not registered
shared-font: [s2pc sm   ] W: GetService(pl:u): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `pl:u` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only nx-shell --results /tmp/r.json
scripts/action_run.sh build/compat/nx-shell/nx-shell.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:pl:u -->`

## create: [compat] service:ssl: needed by curl-example, hb-appstore

labels: compat

**Blocker** `service:ssl`, hit by 2 apps in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 2 of them.

### What it is

The app asks the service manager (`sm:`) for the `ssl` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `ssl` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **switch-examples: network/curl** ([switchbrew/switch-examples@v20260201](https://github.com/switchbrew/switch-examples/tree/v20260201)), level `renders`, 1 frames, first blocker
- **hb-appstore** ([vgmoose/hb-appstore@v2.3.2](https://github.com/vgmoose/hb-appstore/tree/v2.3.2)), level `renders`, 1 frames, first blocker

### Log

```
curl-example: [s2pc sm   ] W: GetService(ssl): not registered
hb-appstore: [s2pc sm   ] W: GetService(ssl): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `ssl` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_net/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only curl-example --results /tmp/r.json
scripts/action_run.sh build/compat/curl-example/curl-example.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:ssl -->`

## create: [compat] service:ts: needed by hbmenu, switchident

labels: compat

**Blocker** `service:ts`, hit by 2 apps in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 1 of them.

### What it is

The app asks the service manager (`sm:`) for the `ts` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `ts` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **nx-hbmenu** ([switchbrew/nx-hbmenu@v3.6.1](https://github.com/switchbrew/nx-hbmenu/tree/v3.6.1)), level `runs`, 376 frames, first blocker
- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
hbmenu: [s2pc sm   ] W: GetService(ts): not registered
switchident: [s2pc sm   ] W: GetService(ts): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `ts` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only hbmenu --results /tmp/r.json
scripts/action_run.sh build/compat/hbmenu/hbmenu.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:ts -->`

## create: [compat] ipc:applet:ICommonStateGetter:40: needed by switchident

labels: compat, good first issue

**Blocker** `ipc:applet:ICommonStateGetter:40`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app sends command 40 to the `applet:ICommonStateGetter` interface and switch2pc's implementation of that interface does not handle it (`hos_ipc_unimplemented` in `shims/hos_cpu/ipc.c` logs it and answers with `NOT_IMPLEMENTED`). The command's name and layout are on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
switchident: [s2pc ipc  ] W: applet:ICommonStateGetter: command 40 not implemented (in 0 bytes, 0 buffers)
```

### Where it goes

A new `case 40:` in `common_state_dispatch` in `shims/hos_os/applet.c`: read the request with `hos_ipc_in_read`, write the reply with `hos_ipc_out_write`, return a `hos_result`; see the neighbouring cases and the test in `tests/test_ipc.c`.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:ipc:applet:ICommonStateGetter:40 -->`

## create: [compat] ipc:applet:ILibraryAppletCreator:10: needed by software-keyboard

labels: compat, good first issue

**Blocker** `ipc:applet:ILibraryAppletCreator:10`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app sends command 10 to the `applet:ILibraryAppletCreator` interface and switch2pc's implementation of that interface does not handle it (`hos_ipc_unimplemented` in `shims/hos_cpu/ipc.c` logs it and answers with `NOT_IMPLEMENTED`). The command's name and layout are on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **switch-examples: applet/libapplets/software-keyboard** ([switchbrew/switch-examples@v20260201](https://github.com/switchbrew/switch-examples/tree/v20260201)), level `runs`, 293 frames

### Log

```
software-keyboard: [s2pc ipc  ] W: applet:ILibraryAppletCreator: command 10 not implemented (in 8 bytes, 0 buffers)
```

### Where it goes

A new `case 10:` in `stub_dispatch` in `shims/hos_os/applet.c`: read the request with `hos_ipc_in_read`, write the reply with `hos_ipc_out_write`, return a `hos_result`; see the neighbouring cases and the test in `tests/test_ipc.c`.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only software-keyboard --results /tmp/r.json
scripts/action_run.sh build/compat/software-keyboard/software-keyboard.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:ipc:applet:ILibraryAppletCreator:10 -->`

## create: [compat] ipc:fsp-srv:11: needed by nx-shell

labels: compat, good first issue

**Blocker** `ipc:fsp-srv:11`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 1 of them.

### What it is

The app sends command 11 to the `fsp-srv` interface and switch2pc's implementation of that interface does not handle it (`hos_ipc_unimplemented` in `shims/hos_cpu/ipc.c` logs it and answers with `NOT_IMPLEMENTED`). The command's name and layout are on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **NX-Shell** ([joel16/NX-Shell@4.01](https://github.com/joel16/NX-Shell/tree/4.01)), level `boots`, 0 frames, first blocker

### Log

```
nx-shell: [s2pc ipc  ] W: fsp-srv: command 11 not implemented (in 4 bytes, 1 buffers)
```

### Where it goes

A new `case 11:` in `fsp_dispatch` in `shims/hos_fs/fsp.c`: read the request with `hos_ipc_in_read`, write the reply with `hos_ipc_out_write`, return a `hos_result`; see the neighbouring cases and the test in `tests/test_ipc.c`.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only nx-shell --results /tmp/r.json
scripts/action_run.sh build/compat/nx-shell/nx-shell.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:ipc:fsp-srv:11 -->`

## create: [compat] ipc:fsp-srv:61: needed by save-example

labels: compat, good first issue

**Blocker** `ipc:fsp-srv:61`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 1 of them.

### What it is

The app sends command 61 to the `fsp-srv` interface and switch2pc's implementation of that interface does not handle it (`hos_ipc_unimplemented` in `shims/hos_cpu/ipc.c` logs it and answers with `NOT_IMPLEMENTED`). The command's name and layout are on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **switch-examples: fs/save** ([switchbrew/switch-examples@v20260201](https://github.com/switchbrew/switch-examples/tree/v20260201)), level `runs`, 229 frames, first blocker

### Log

```
save-example: [s2pc ipc  ] W: fsp-srv: command 61 not implemented (in 0 bytes, 0 buffers)
```

### Where it goes

A new `case 61:` in `fsp_dispatch` in `shims/hos_fs/fsp.c`: read the request with `hos_ipc_in_read`, write the reply with `hos_ipc_out_write`, return a `hos_result`; see the neighbouring cases and the test in `tests/test_ipc.c`.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only save-example --results /tmp/r.json
scripts/action_run.sh build/compat/save-example/save-example.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:ipc:fsp-srv:61 -->`

## create: [compat] ipc:set:sys:68: needed by switchident

labels: compat, good first issue

**Blocker** `ipc:set:sys:68`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app sends command 68 to the `set:sys` interface and switch2pc's implementation of that interface does not handle it (`hos_ipc_unimplemented` in `shims/hos_cpu/ipc.c` logs it and answers with `NOT_IMPLEMENTED`). The command's name and layout are on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
switchident: [s2pc ipc  ] W: set:sys: command 68 not implemented (in 0 bytes, 0 buffers)
```

### Where it goes

A new `case 68:` in `setsys_dispatch` in `shims/hos_os/setsys.c`: read the request with `hos_ipc_in_read`, write the reply with `hos_ipc_out_write`, return a `hos_result`; see the neighbouring cases and the test in `tests/test_ipc.c`.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:ipc:set:sys:68 -->`

## create: [compat] service:acc:su: needed by jksv

labels: compat

**Blocker** `service:acc:su`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 1 of them.

### What it is

The app asks the service manager (`sm:`) for the `acc:su` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `acc:su` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **JKSV** ([J-D-K/JKSV@d39fd80a208b7c802d12edb12f362506a0addb47](https://github.com/J-D-K/JKSV/tree/d39fd80a208b7c802d12edb12f362506a0addb47)), level `boots`, 0 frames, first blocker

### Log

```
jksv: [s2pc sm   ] W: GetService(acc:su): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `acc:su` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only jksv --results /tmp/r.json
scripts/action_run.sh build/compat/jksv/jksv.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:acc:su -->`

## create: [compat] service:gpio: needed by switchident

labels: compat

**Blocker** `service:gpio`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app asks the service manager (`sm:`) for the `gpio` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `gpio` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
switchident: [s2pc sm   ] W: GetService(gpio): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `gpio` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:gpio -->`

## create: [compat] service:hid:dbg: needed by switchident

labels: compat

**Blocker** `service:hid:dbg`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app asks the service manager (`sm:`) for the `hid:dbg` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `hid:dbg` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
switchident: [s2pc sm   ] W: GetService(hid:dbg): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `hid:dbg` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:hid:dbg -->`

## create: [compat] service:ns:am2: needed by switchident

labels: compat

**Blocker** `service:ns:am2`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app asks the service manager (`sm:`) for the `ns:am2` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `ns:am2` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
switchident: [s2pc sm   ] W: GetService(ns:am2): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `ns:am2` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:ns:am2 -->`

## create: [compat] service:ns:ec: needed by switchident

labels: compat

**Blocker** `service:ns:ec`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app asks the service manager (`sm:`) for the `ns:ec` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `ns:ec` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
switchident: [s2pc sm   ] W: GetService(ns:ec): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `ns:ec` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:ns:ec -->`

## create: [compat] service:ns:rid: needed by switchident

labels: compat

**Blocker** `service:ns:rid`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app asks the service manager (`sm:`) for the `ns:rid` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `ns:rid` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
switchident: [s2pc sm   ] W: GetService(ns:rid): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `ns:rid` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:ns:rid -->`

## create: [compat] service:ns:ro: needed by switchident

labels: compat

**Blocker** `service:ns:ro`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app asks the service manager (`sm:`) for the `ns:ro` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `ns:ro` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
switchident: [s2pc sm   ] W: GetService(ns:ro): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `ns:ro` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:ns:ro -->`

## create: [compat] service:ns:rt: needed by switchident

labels: compat

**Blocker** `service:ns:rt`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app asks the service manager (`sm:`) for the `ns:rt` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `ns:rt` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
switchident: [s2pc sm   ] W: GetService(ns:rt): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `ns:rt` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:ns:rt -->`

## create: [compat] service:ns:web: needed by switchident

labels: compat

**Blocker** `service:ns:web`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app asks the service manager (`sm:`) for the `ns:web` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `ns:web` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
switchident: [s2pc sm   ] W: GetService(ns:web): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `ns:web` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:ns:web -->`

## create: [compat] service:set:cal: needed by switchident

labels: compat

**Blocker** `service:set:cal`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 1 of them.

### What it is

The app asks the service manager (`sm:`) for the `set:cal` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `set:cal` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames, first blocker

### Log

```
switchident: [s2pc sm   ] W: GetService(set:cal): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `set:cal` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:set:cal -->`

## create: [compat] service:spl:: needed by switchident

labels: compat

**Blocker** `service:spl:`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app asks the service manager (`sm:`) for the `spl:` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `spl:` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
switchident: [s2pc sm   ] W: GetService(spl:): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `spl:` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_os/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:spl: -->`

## create: [compat] service:wlan:inf: needed by switchident

labels: compat

**Blocker** `service:wlan:inf`, hit by 1 app in the nightly compatibility run of 2026-10-13T02:30:28Z (switch2pc 2bb3907); the first blocker in 0 of them.

### What it is

The app asks the service manager (`sm:`) for the `wlan:inf` service and switch2pc has no implementation of it, so `GetService` fails with "not registered". Most homebrew tolerate a missing service for a while and fail later, when they use it. What `wlan:inf` does is on [switchbrew](https://switchbrew.org/wiki/Services_API).

### Apps that stop at it

- **SwitchIdent** ([joel16/SwitchIdent@0.5](https://github.com/joel16/SwitchIdent/tree/0.5)), level `runs`, 12 frames

### Log

```
switchident: [s2pc sm   ] W: GetService(wlan:inf): not registered
```

### Where it goes

A new `hos_service_class` registered under the name `wlan:inf` from a library constructor with `hos_ipc_register_service` (CONTRIBUTING.md, "Adding a service"); `shims/hos_os/lm.c` is the smallest complete example and `shims/hos_net/` is where it most likely belongs. `shims/hos_cpu/ipc_sm.c` logs the miss. Start with the commands the apps below actually call: run the app with `HOS_LOG_LEVEL=debug` and look for the `command N not implemented` lines that follow.

### How to check a fix

Build the app with `compat/build_app.sh <name> build/compat/<name>` in the `devkitpro/devkita64` container, then run it the way the nightly does:

```
compat/run.py --nro-dir build/compat --only switchident --results /tmp/r.json
scripts/action_run.sh build/compat/switchident/switchident.nro --duration 8000 --frames out
```

The log line above should be gone and the app's level should not drop; see compat/README.md for the levels. This issue is maintained by the nightly run (compat/issues.py): it is updated while apps hit the blocker and closed when none does.

`<!-- compat:service:wlan:inf -->`
