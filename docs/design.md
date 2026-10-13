# Design notes

## 1. What the first brief got wrong

The original plan was a "relinker": parse the game as an ARM64 ELF, rewrite its
relocations and dynamic section, emit a Linux ELF whose `DT_NEEDED` entries name
`libhos_*.so`, and let `ld.so` bind imports such as `svcThreadCreate` or
`FsOpenFile` to our shims. Default target: x86-64.

None of those premises hold for a Switch binary.

1. **There is no ELF and there are no imports.** The executable is an NSO0:
   three LZ4 segments plus bss, and a MOD0 header pointing at a `.dynamic`
   section used by the SDK's own runtime linker. The Nintendo SDK (`nnSdk`) is
   statically linked. Kernel calls are the instruction `svc #n`, not a call
   through a PLT. There is no symbol named `svcThreadCreate` to bind.
2. **System services are IPC, not functions.** Filesystem, audio, input and
   GPU are separate processes on HOS. The game reaches them with
   `svcSendSyncRequest` on a session handle, with the request marshalled into
   the thread's TLS block. A shim must speak that protocol.
3. **The GPU is the hard part and it is not an API.** NVN lives inside the
   game. It produces Maxwell pushbuffers through `nvdrv` ioctls. Mapping "NVN
   calls" to Vulkan means emulating a GPU command processor and translating
   shaders. That is most of an emulator and this project does not build one.
4. **ARM64 code does not run on x86-64.** The brief made x86-64 the default and
   the recompiler optional. One of those had to go. We keep ARM64 hosts only.
5. **`svc` on Linux is a Linux syscall.** The kernel ignores the immediate and
   dispatches on `x8`. Every `svc` site has to be rewritten or trapped.
6. **Decrypting NCAs is what Nintendo sued over.** The `despack` module would
   have been the one piece with real legal exposure. It is out of scope; users
   provide an already extracted NSO.

## 2. What we build instead

A Wine-style loader for Linux ARM64. The game is the process; `hosrun` is a
small ELF that maps the module and jumps into it. `libhos_cpu.so` is the kernel
the game thinks it has. Later libraries (`libhos_fs`, `libhos_hid`, ...) will be
the IPC services, each answering requests on the ports the game connects to.

Modularity is real, not cosmetic: each `libhos_*.so` is a separate shared
object that the loader lists in `DT_NEEDED`. Replacing one with your own build
needs no relink.

## 3. Mechanisms

### Patching

Three one-word rewrites, applied in memory after loading (or on disk with
`switch2pc patch`):

| Before                 | After                 | Why                                                 |
|------------------------|-----------------------|-----------------------------------------------------|
| `svc #n` (1..0x7F)     | `brk #n`              | SIGTRAP with the svc number recoverable from the PC |
| `mrs Xt, tpidrro_el0`  | `mrs Xt, tpidr_el0`   | Linux leaves tpidrro_el0 at 0 for 64-bit tasks       |
| `mrs Xt, cntpct_el0`   | `mrs Xt, cntvct_el0`  | EL0 cannot read the physical counter on Linux        |

Code size and all relative branches are unchanged. The scanner looks at every
aligned word in `.text`; a data word that happens to encode `svc` would be
rewritten too (#8). Real modules keep literal pools in `.rodata`, so this has
not been a problem in practice, but a function-boundary aware pass is planned.

### Trap and dispatch

`brk` raises SIGTRAP. The handler is an assembly stub (`trap_aarch64.S`) because
the kernel does not save `tpidr_el0` in the signal frame and the game's TLS
block is in that register. The stub checks whether `tpidr_el0` points inside
the TLS arena; if so it swaps the host TCB back in (stored at offset `0x110` of
the block), calls `hos_trap_c`, then restores the game block before returning.
`hos_trap_c` reads the `brk` immediate at the faulting PC, copies `x0..x7` into
`hos_svc_regs`, calls `hos_svc_dispatch`, copies them back and advances the PC.

The same stub serves SIGSEGV, SIGBUS, SIGILL and SIGFPE so a crash in game
code prints registers and the module-relative PC, then exits with `128+sig`.

Cost is one signal round trip per svc, a few microseconds. Good enough to boot
anything; replacing hot sites with direct trampolines is #5.

### Threads

`svcCreateThread` creates a detached pthread with a small host stack. The
pthread waits on a semaphore until `svcStartThread`, binds itself (allocates a
TLS block, records the host TCB in it, installs a sigaltstack, clears the
signal mask), then calls `hos_enter`, which loads `tpidr_el0`, moves `sp` to
the game-supplied stack and branches. `svcExitThread` arrives as a trap and
`siglongjmp`s back to the pthread body, which frees everything and returns.
The main thread does the same from `hos_cpu_run_main`; if it exits first,
`pthread_exit` keeps the process alive for the others.

### Memory

HOS gives applications a fixed 39-bit layout and `nnSdk` asks about it through
`svcGetInfo` before doing anything else. We reserve the regions up front with
`PROT_NONE` mappings and report those addresses:

| Region    | Address          | Size   |
|-----------|------------------|--------|
| image     | `0x8000000`      | module |
| heap      | `0x1000000000`   | 4 GiB  |
| alias     | `0x2000000000`   | 4 GiB  |
| stack     | `0x3000000000`   | 4 GiB  |
| TLS arena | `0x3800000000`   | 64 MiB |

`SetHeapSize` flips pages inside the heap reservation between `PROT_NONE` and
read/write. `MapMemory` is `mremap(MREMAP_FIXED)`: the pages move, the source
becomes inaccessible, which is exactly the HOS semantics. A sorted region table
(`mem_regions.c`) answers `QueryMemory`, including free gaps.

### TLS block layout

See `shims/hos_cpu/tls_layout.h`. Game-visible fields match hardware; our two
words live at `0x110` and `0x118`, inside the range HOS marks reserved. If a
game is found writing there, move them.

### Synchronisation

`sync.c` is one lock and one wait list. A blocked thread sits in a `waiter`
on its own host stack with a per-waiter condition variable; whoever wakes it
removes it from the list first, so a timeout and a wake cannot both claim it.
The list is in arrival order and a wake picks the highest-priority waiter on
the key (lowest number, as the kernel does), the earliest arrival among equals,
reading the thread's priority at that moment rather than at park time, so a
`SetThreadPriority` on a parked thread moves it at once, behind the waiters
of its new priority, as the kernel re-inserts it.
Mutexes follow the kernel contract exactly: the user word holds the owner
handle, bit 30 means "someone is parked", `ArbitrateUnlock` hands the word to
the next waiter. `WaitProcessWideKeyAtomic` releases the mutex, parks on the
condvar key, and on signal either takes the mutex outright or is moved onto the
mutex queue. On timeout the caller gets `TimedOut` without the mutex, as on
hardware. Events, thread exit and `WaitSynchronization` use the same list with
the object pointers resolved up front; `SignalProcessWideKey` clears the
condvar key when the queue empties, which is what the SDK fast path checks.

### IPC

Every game-visible service is an object (`hos_object`) of a class
(`hos_service_class`) reached through a session handle. `ipc.c` decodes the
HIPC message in the TLS block, frames CMIF (16-byte aligned SFCI/SFCO, control
commands, domains with object ids) or TIPC (payload at the start of the raw
data, type = 16 + command), calls the class dispatch function and writes the
reply back into the same block. Service libraries register classes by name
from a constructor; `sm:` instantiates one on `GetService`. A single request
lock serialises everything for now (#16).

Addresses in buffer descriptors are 39 bits wide, which is the whole HOS user
address space. Anything a game passes lives inside the regions we reserve, so
this is only a constraint on host-side tests, which must use the HOS heap for
buffers they hand to a service.

### What traps cost

`HOS_TRAP_STATS=1` counts every trap by kind and prints totals and rates at
exit. Measured under qemu-user on a desktop CPU:

| Game   | counter reads/s | svcs/s |
|--------|-----------------|--------|
| Tyrian | 10 557          | 541    |
| DOOM   | 531             | 414    |
| Quake  | 2 477           | 1 426  |

Quake first showed 371k counter reads and 93k WaitSynchronization a second:
`Host_FilterTime` drops frames until 1/72 s has passed and the loop around
it spun, five traps per turn. That puts the cost of one trap at about 2 us
or less, so even Tyrian spends roughly 2% of a core in traps. Trampolines
(#5, #23) would save that and cost a rewrite of game code at every site, so
they wait for a title that traps an order of magnitude more.

### Several modules

A retail exefs holds `rtld`, `main`, `subsdk0..9` and `sdk`. The kernel maps
them back to back in that order and enters `rtld`, which walks memory with
`svcQueryMemory`, finds each module's MOD0 and binds symbols between them.
`hosrun` reserves one range at `0x8000000`, maps the modules contiguously with
`nso_load_at`, and runs `mod0_link`, which resolves every undefined symbol
against the other modules' dynamic symbol tables (symbol counts from `DT_HASH`
or `DT_GNU_HASH`). When `rtld` is present it repeats that work itself; the
results are identical, so the double pass is harmless. Without `rtld` our link
is the only one, which is what the `multi_module` example exercises.

### Services

Each service library registers classes from a constructor. `set:sys` reports
firmware 11.0.1 (below the 12.0 line where libnx switches `sm` to TIPC, which
we also support). `applet` plays an always-focused handheld application: one
`FocusStateChanged` message, then `NoMessage`. `hid` hands out a zeroed 256 KiB
shared memory. `time` answers from the host clocks. `fsp-srv` maps the SD card
and save data onto host directories given by `--sdmc` and `--save`, and serves
the romfs image given by `--romfs` as the data storage; paths are sanitised so
nothing escapes the mount root.

### Time

Games and libnx hardcode the Switch counter frequency, 19.2 MHz, when turning
`cntvct_el0` into seconds, and the host counter runs at whatever the SoC (or
qemu) provides. The patcher therefore rewrites counter reads into `brk`
instructions whose immediate carries the destination register; the trap
returns `CLOCK_MONOTONIC` scaled to 19.2 MHz and `cntfrq_el0` reads return the
constant. `svcGetSystemTick`, the steady clock IPC and the time shared memory
all derive from the same monotonic base, so every way of asking for the time
agrees. A trap per read is slow (#23); correctness first.

### Relocations the module owns

libnx and nnrtld relocate their own images at start-up. For RELA that is
idempotent and our earlier pass is harmless. RELR entries add the base to the
word in place, so applying them twice corrupts the image. The rule is
therefore about who else will do it: with rtld in the set, rtld relocates
and initialises every module, and the loader only counts RELR; without rtld,
module 0 (libnx's crt0) still relocates and initialises itself, but nobody
would touch the libraries, so the loader applies their RELR (#22) and runs
their DT_INIT and DT_INIT_ARRAY (#21). It does the latter without a second
execution path: it writes one page of ARM64 code that saves the entry
arguments, calls each initialiser (dependencies first: sdk, then subsdk9..0),
restores the arguments and branches to the real entry, and makes that page
the entry point. Initialisers thus run on the main thread with its stack and
HOS TLS, as under rtld. `examples/multi_module` covers both: its sdk is linked
with lld's `-z pack-relative-relocs` (GNU ld 2.42 ignores it on aarch64) and
has a constructor that `main` checks.

### Display

The runtime never draws through an API we could intercept; it allocates a
buffer from `nvmap`, draws into it with the CPU (or the GPU, later), and hands
it to the compositor through `vi`: an `IHOSBinderDriver` session carrying
Android parcels for `IGraphicBufferProducer` (`Connect`,
`SetPreallocatedBuffer`, `DequeueBuffer`, `QueueBuffer`). `libhos_gpu` speaks
that protocol with a null compositor: `QueueBuffer` marks the slot free again
at once and, with `HOS_FRAMES` set, reads the pixels straight out of the nvmap
allocation (which is game memory in this process), undoes the Tegra 16Bx2
block-linear swizzle and writes a PPM. Two details cost real time: with
`HipcAutoSelect` the client sends both A/B and X/C descriptors and nulls the
unused pair, so buffer lookups must skip null entries; and `DequeueBuffer` must
always return a flattened `NvMultiFence` (with zero fences), because the client
does not initialise its copy when the reply says there is none.

Vsync (#26): the console's compositor shows one frame per 60 Hz refresh, so
`QueueBuffer` holds the presenting thread until the next refresh, with both the
vi lock and the IPC request lock released meanwhile, and the vsync event
(`GetDisplayVsyncEvent`) fires on the same grid. Refreshes are absolute
deadlines on the monotonic clock, anchored at first use, so a late wake never
shifts the next one. A game faster than 60 fps runs at 60; a slower one lands
on the refresh after it finishes, as on hardware. `HOS_VSYNC=0` turns the
pacing off, which is how the tests, `scripts/action_run.sh` and the
benchmarks run, since there a frame is worth presenting as soon as it exists.

### Window

`HOS_WINDOW=1` makes `vi` present each decoded frame in an X11 window
(`display_x11.c`), which WSLg shows on the Windows desktop. Two threads share
one connection under `XInitThreads`: the game thread that queues a buffer
scales it (nearest neighbour, letterboxed) into an `XImage` and puts it, and
an event thread owns the keyboard, resizes and close requests, so input keeps
flowing during loading screens. The keyboard reaches `hid` through a small
"external pad" in `libhos_cpu` (`hos_pad_external_set`), which keeps
`libhos_gpu` and `libhos_input` independent. Closing the window delivers the
applet exit request rather than killing the process, and a custom X error
handler keeps a destroyed window from taking the game down with Xlib's
default exit. The target's libX11 comes from Ubuntu's arm64 ports; the cross
toolchain file searches the multiarch directory for it.

### Sockets

`bsd:u` maps guest descriptors to host sockets. libnx passes its FreeBSD
headers' values straight through, so sockaddr (with its length byte), MSG_*,
SOL_SOCKET/SO_*, IP_* and POLLWR* are translated; two values were found only
by running libnx against the service: fcntl's O_NONBLOCK arrives as 0x800,
and errno is expected in Linux numbering (libnx converts it to newlib's), so
the host errno passes through untouched. A first version translated errno to
FreeBSD's and libnx turned EAGAIN into EDEADLK. Calls that wait on the
network use `hos_ipc_blocking_begin/end` to drop the IPC request lock, or one
thread's `recv` would freeze every other thread's service calls.

### Input

hid never pushes events; the game polls a shared memory. Each Npad holds
several LIFOs (one per controller style) of 17 samples with a header (tail,
count). The writer publishes a sample by filling the next entry, then its
sampling number, then the tail, with release stores, so a reader that checks
the two sampling numbers never sees a torn entry. Offsets were measured with
the devkitA64 compiler against libnx's `HidSharedMemory` rather than copied
from documentation: player 1 at `0x9A00`, full-key LIFO at `+0x28`, 48-byte
entries. Sources are a timed script and Linux evdev; the former is what the
tests use.

### Audio

A game that waits for its audio buffers must wait about as long as the
hardware would, or threads that assume they will block spin instead. With
`HOS_AUDIO` set, each audout stream gets a host output (PulseAudio, then
ALSA, both through `dlopen`) and a sink thread that writes the queued
buffers in order and releases each one when the device has accepted it; the
blocking write is what paces the game. Without an output a timer pacer
plays that part: a buffer is released when its samples would have finished
(`cursor = max(cursor, now) + samples / rate`). The first version released
buffers at once, which looked harmless with a game that mixes on its main
thread and was fatal with SDL: its audio thread never blocked, held the
single IPC lock most of the time (#16) and the main thread fell to a frame
per second. Output was verified by recording the WSLg sink's monitor while
`sdl_test` played its tone: one second at 440 Hz came back.

### SDL2

An SDL2 game needs no platform code once SDL itself runs here, so
`ports/sdl2/` carries a video driver for devkitPro's Switch fork of SDL 2.28:
the window is a libnx `Framebuffer`, the software renderer draws into an SDL
surface that `UpdateWindowFramebuffer` copies into the next buffer, and the
audio (audout), joystick (hid), thread and filesystem backends are the fork's
own. `ports/sdl2/s2pc_prelude.c` is linked into games to mount romfs and send
stdout/stderr to the log through a devoptab, so a stock `main()` works. One
build detail matters: the fork turns `SDL_CPUINFO` off for the Switch, and
SDL's audio converters are then never chosen on ARMv8 (it assumes NEON and
compiles no scalar fallback), so the build forces it on.

## 4. Roadmap

Phase 1 (kernel): done. Synchronisation (#3), `sm:` and the IPC transport
(#9), the service set a runtime needs at start-up (#14, #7, #13), multi-module
loading with cross-module symbols (#2, #10), the time model (#17), and the
exit criterion itself: a libnx hello world runs its full runtime initialisation
and reaches `main` (#18). Still open from this phase: `GetInfo` program id
(#4), SHA-256 segment hashes (#1), init arrays without rtld (#21), RELR for
non-self-relocating modules (#22).

Phase 2 (services): `hid`, `vi` with a null compositor, paced `audout`,
`nvdrv` returning errors cleanly. Exit criterion: a 2D homebrew runs headless.
Done: DOOM, Quake and an unmodified SDL2 game (OpenTyrian) run in a real
window (#25) with real sound (#27).

Phase 3 (GPU): decided for a Maxwell command processor. The homebrew this
project runs reaches the GPU through deko3d or Mesa, which both submit
Maxwell pushbuffers through `nvhost-gpu`; NVN exists only inside retail
games, so hooking it is out of scope. Milestones M0 (the driver accepts the
GPU) to M5 are in [gpu.md](gpu.md).

## 5. Non-goals

x86-64 and Windows targets, decryption of any kind, a monolithic runtime,
anything that requires Nintendo's keys or SDK.
