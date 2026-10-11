# Issue tracker

Numbered so `FIXME(#n)` in the source means something before the repository has
a public issue tracker. Move these to GitHub issues when it does, keeping the
numbers.

| #  | Area    | Title                                                         | Status |
|----|---------|---------------------------------------------------------------|--------|
| 1  | nso     | Verify SHA-256 segment hashes when the header flags ask for it | done (sha256.c; nso.c verifies text/rodata/data) |
| 2  | mod0    | Resolve symbols across modules (main, sdk, subsdk*)            | done (mod0_link; DT_HASH/GNU_HASH symbol counts) |
| 3  | hos_cpu | Events, WaitSynchronization, ArbitrateLock/Unlock, ProcessWideKey, WaitForAddress | done (sync.c; pthread cond per waiter, FIFO wake) |
| 4  | hos_cpu | GetInfo ProgramId: carry the title id from the package into the loader | done (--program-id <hex>) |
| 5  | hos_cpu | Replace brk/SIGTRAP with direct trampolines for hot svc sites. Design notes: `b stub_n` per site, stub saves x8-x18, x29, x30 and v0-v31 (the kernel preserves them, C code does not), swaps tpidr_el0, calls the dispatcher, restores, branches back. Stub pages must sit within 128 MiB of the site and must not look like a module to nnrtld's memory walk; deferred until a real game can be profiled | deferred (measured: a trap costs at most ~2 us under qemu; Tyrian makes ~11k/s, about 2% of a core, DOOM ~1k/s; revisit for a title that traps far more) |
| 6  | config  | TOML `game.toml`, defaults and per-title profiles              | open   |
| 7  | hos_fs  | `fsp-srv`: sdmc and save data on host directories, romfs from an image file | done (fsp.c) |
| 8  | patch   | Function-boundary aware scan so data words encoding `svc` are not rewritten | open |
| 9  | hos_cpu | `sm:` service manager and ConnectToNamedPort                   | done (ipc.c, ipc_sm.c; CMIF, TIPC, domains) |
| 10 | loader  | Load several NSOs (rtld, main, sdk, subsdk0-9) at adjacent bases | done (hosrun; exefs directory or explicit list) |
| 11 | tests   | libFuzzer harness for nso_load, mod0_relocate and hipc_parse   | open   |
| 12 | ci      | GitHub Actions: host build + cross build + ctest under qemu    | open   |
| 13 | hos_cpu | Shared memory and transfer memory svcs (0x13-0x15, 0x50)        | done (svc_shmem.c; MapTransferMemory 0x51/0x52 still open) |
| 14 | hos_os  | `applet` (appletOE/AE), `time`, `set:sys` stubs so runtime init completes | done (applet.c, time.c, setsys.c) |
| 15 | hos_cpu | Priority-ordered wake in sync.c instead of FIFO                | open   |
| 16 | hos_cpu | Per-service locking instead of the single IPC request lock      | open (no contention measured: ~180 IPC requests/s in the games we run, once audout stopped spinning) |
| 17 | hos_os  | Publish steady/system clock contexts in the time shared memory  | done (time.c; steady base 0, matches the 19.2 MHz counter) |
| 18 | tests   | Run a libnx hello world                                        | done (examples/libnx_hello, smoke_libnx_hello; devkitA64 installed on Windows, scripts/build_libnx_hello.sh) |
| 22 | mod0    | Apply DT_RELR for modules that do not self-relocate | done (mod0_module.apply_relr; hosrun sets it for library modules when no rtld is loaded) |
| 23 | patch   | Counter reads trap on every `mrs cntvct_el0`; an inline scaled read needs a per-site stub (no free registers, LR is live) | deferred (same measurement as #5: ~10k reads/s in the busiest game, ~2% of a core) |
| 24 | hos_gpu | nvhost-gpu / nvhost-as-gpu / nvhost-ctrl-gpu | done for M0 (see #40); drawing is M1+ |
| 25 | hos_gpu | A window: present queued frames in real time | done (display_x11.c, HOS_WINDOW=1: scaled X11 window via WSLg, keyboard as player 1 through hos_pad_external_set, close = applet exit request) |
| 26 | hos_gpu | Vsync pacing: QueueBuffer returns immediately; hold the buffer until the next vsync so games run at 60 Hz | open |
| 27 | hos_audio | Real-time audio output | done (audio_sink.c: PulseAudio or ALSA via dlopen, HOS_AUDIO=auto/pulse/alsa; a sink thread per stream releases buffers at the device clock; timer pacing when off; verified by recording the WSLg sink monitor) |
| 28 | examples | DOOM (doomgeneric) runs end to end; fetch of third_party/doomgeneric and the IWAD is manual, document it in scripts/build_doom.sh | done |
| 29 | hos_gpu  | HOS_FRAMES_EVERY=N to dump every Nth frame; a 640x400 PPM per frame is heavy on slow filesystems | done |
| 30 | tools    | Live play on Windows: HOS_FRAMES_LIVE (BMP, atomic rename) + HOS_INPUT_FILE + tools/viewer.ps1 + scripts/run_game.sh. Stopgap for #25 | done |
| 31 | examples | Quake (quakegeneric, 64-bit, software renderer) runs its demo loop; scripts/build_quake.sh; smoke_quake | done |
| 32 | hos_cpu  | Timing under qemu: measure before trampolines | done (HOS_TRAP_STATS=1 prints trap counts and rates at exit; Quake's 464k traps/s was its own busy loop, fixed in examples/quake: 3.9k/s now) |
| 33 | ports    | SDL2 for the runtime: software video driver over devkitPro's Switch SDL 2.28 (ports/sdl2, scripts/build_sdl2.sh), SDL_CPUINFO forced on so audio converters exist on ARMv8; examples/sdl_test + smoke_sdl_test | done |
| 34 | examples | OpenTyrian, an unmodified SDL2 game, runs from romfs data (scripts/build_opentyrian.sh, ports/sdl2/s2pc_prelude.c for romfsInit + stdio to the log); smoke_tyrian | done |
| 35 | hos_os   | Applet exit request: `hos_applet_request_exit()` delivers AppletMessage_ExitRequest; the input script's `<ms> exit` line uses it, so games leave through their own quit path | done |
| 36 | ports    | SDL2 keyboard/mouse/touch pollers come from the devkitPro driver and read hid directly; they are compiled but unexercised (no keyboard in the hid shared memory yet) | open |
| 37 | ports    | SDL2 window is always the size the game asked for | done (the X11 window scales and letterboxes any frame size) |
| 38 | hos_os   | `acc:u0`/`acc:u1` (one local user, IProfile) and `psm` (battery, sessions) for homebrew that asks for a user or the battery | done (account.c, psm.c; checked through libnx's own client in libnx_services) |
| 39 | hos_net  | `bsd:u` sockets over host sockets | done (bsd.c: IPv4 TCP/UDP, poll, fcntl, socket options; FreeBSD sockaddr/MSG/SO/IP constants translated, errno in Linux numbering and O_NONBLOCK 0x800 as libnx expects; blocking calls drop the IPC lock; smoke_libnx_sockets) |
| 41 | hos_net  | `sfdnsres` lookups (GetAddrInfo/GetHostByName serialisation) over the host resolver; Select/Ioctl/Sysctl in bsd:u | open |
| 40 | hos_gpu  | GPU for homebrew (deko3d, Mesa): nvhost-gpu channels, GPFIFO and a Maxwell pushbuffer decoder feeding Vulkan; milestones M0-M5 in docs/gpu.md | open (M0 done: nvgpu.c, deko3d device/queue/swapchain/submit/present/teardown, smoke_deko3d_m0; next M1 clears and copies) |
| 19 | hos_fs  | Build a romfs image from a directory at load time               | done (romfs_build.c; lazy, file data read on demand) |
| 20 | hos_input | Controller input into the hid shared memory                   | done (input_state.c: 60 Hz writer; HOS_INPUT_SCRIPT, HOS_INPUT_EVDEV; evdev path compiled but untested without a device) |
| 21 | loader  | Call DT_INIT_ARRAY of library modules when no rtld is present   | done (mod0_init_functions + a one-page ARM64 trampoline that runs them before the entry) |
