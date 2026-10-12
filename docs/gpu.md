# GPU plan (#40)

Status: **M3b done** (2026-10-12): a 3D cube. `examples/libnx_deko3d_cube`
draws deko3d's Example03 cube at a fixed rotation: 24 vertices from a vertex
buffer, model-view and projection matrices in a uniform buffer uploaded
through the command stream, quads, back-face culling. It matches an
independent double-precision reference (`check_deko3d_cube`: 0 pixels off by
more than 2, worst 1); the triangle of M3a still matches exactly. M0, M1,
M3a and M3b are done, all on the CPU. Next: M3c (depth buffer, blending,
more of deko3d's examples), then textures (M4).

## Which path, and why

Homebrew that uses the GPU goes through one of two libraries, both
open source and both shipped by devkitPro:

- **deko3d**, the native low-level API (`libdeko3d.a` is already installed).
- **Mesa** (nouveau, through EGL/OpenGL), used by SDL2's GL renderer and
  OpenGL homebrew.

Both build Maxwell (GM20B) command buffers themselves and hand them to the
kernel driver through `nvhost-gpu`'s GPFIFO. Neither uses NVN. So for the
homebrew this project runs, the GPU work is a Maxwell command processor:
decode the pushbuffers and execute them on the host GPU. Hooking NVN would
only help retail games, which are out of scope (see the scope rules in
docs/design.md), so it is not part of this plan.

## Architecture

```
game (deko3d / mesa)
  | ioctls on /dev/nvhost-as-gpu, /dev/nvhost-gpu, /dev/nvhost-ctrl-gpu
  v
hos_gpu: nvdrv
  - address space: GPU VA -> nvmap handle + offset (MAP_BUFFER_EX, UNMAP)
  - channel: GPFIFO ring, SUBMIT_GPFIFO entries (GPU VA + length)
  - syncpoints: fences returned by submit, waited on by nvhost-ctrl
  |
  v
command processor (new, CPU thread per channel)
  - pushbuffer decoder: method headers (incrementing, non-incrementing,
    increment-once, immediate), subchannel -> bound class
  - engines: 3D (0xB197), compute (0xB1C0), DMA copy (0xB0B5),
    2D (0x902D), inline-to-memory
  - state tracker: render targets, viewports, blend, depth, vertex
    streams, constant buffers, TIC/TSC (texture and sampler headers)
  |
  v
backend: Vulkan (host GPU, or lavapipe for tests and CI)
  - surfaces: nvmap memory <-> VkImage, block-linear swizzle both ways
  - shaders: Maxwell SASS -> SPIR-V (the large part)
```

Guest memory is host memory here (the game is the process), so a GPU VA
resolves to a host pointer with one table lookup; nothing is copied until
the backend needs a VkBuffer or VkImage.

## Milestones

Each one ends with an example in `examples/` and a smoke test, like every
other feature in this repository.

1. **M0, the driver accepts the GPU. Done.** `nvhost-ctrl-gpu`
   characteristics (GM20B values) and zcull queries, `nvhost-as-gpu`
   address space and mappings, `nvhost-gpu` channel setup and
   `SUBMIT_GPFIFO`/`SUBMIT_GPFIFO_EX`, `nvhost-ctrl` syncpoint waits and
   per-slot events. Commands are parsed and only the synchronisation subset
   runs. Exit met: a deko3d program creates a device, queue and swapchain,
   submits frames and exits cleanly (`shims/hos_gpu/nvgpu.c`).
2. **M1, clears and copies. Done.** The 3D engine's macro processor (MME),
   colour clears into pitch and block-linear render targets (RGBA8, sRGB,
   BGRA8, RGB565, RGBA32F), the DMA copy engine (pitch and block-linear
   both ways, remapped constant fills), inline-to-memory uploads, report
   semaphores. All on the CPU, in `gpu_engines.c` and `gpu_mme.c`. Exit met:
   the clear colour shows through `vi`.
3. **M2, a Vulkan backend for what M1 does (weeks).** Same results, on the
   host GPU. This is where window and GPU meet; the X11 window gets a
   Vulkan swapchain.
4. **M3, shaders (months).** Decode Maxwell SASS for vertex and fragment
   stages (the instructions deko3d's compiler emits for simple GLSL), vertex
   fetch, constant buffers, rasterisation state; on the CPU first, then
   translated to SPIR-V for M2's Vulkan backend. Split into verifiable
   steps: **M3a, the triangle: done** (`gpu_shader.c` interpreter,
   `gpu_draw.c` rasteriser); **M3b, vertex buffers, uniforms and the cube:
   done**; M3c, depth test, blending and the rest of deko3d's examples.
5. **M4, textures (weeks).** TIC/TSC decoding, texture formats, samplers,
   swizzled texture upload. Exit: a textured quad; then SDL2 through Mesa.
6. **M5, the rest (open-ended).** Compute, queries, transform feedback,
   tessellation and geometry stages, as real homebrew needs them.

## What M0 taught

Each point below cost a failed run and is now in `nvgpu.c`:

- deko3d sets 64 KiB big pages with `INITIALIZE_EX` and checks that every
  mapping is aligned to the page size it asked for.
- An image's compressible kind is applied with `MAP_BUFFER_EX` + Modify
  (0x100) and nvmap handle 0: change the kind of a range, keep the memory.
- Submissions arrive as `SUBMIT_GPFIFO_EX` (nr 0x1B) through Ioctl2, the
  GPFIFO entries in the second input buffer.
- Fence thresholds are computed by deko3d from the syncpoint increments in
  its own pushbuffers, so "signal at once" means a wait raises the
  syncpoint to its threshold, and `QueryEvent` hands out one event per
  (fd, id) that `EVENT_WAIT_ASYNC` on that slot signals.
- `dkQueueWaitIdle` reads a semaphore the GPU writes, so even M0 must run
  semaphore releases: the GPFIFO class's 0x04-0x07, the 3D/compute report
  semaphore at 0x6C0-0x6C3 and the DMA engine's at 0x90-0x92 with
  LAUNCH_DMA. Pushbuffer segments may be a single zero word (a NOP).

## What M1 taught

- deko3d never writes CLEAR_SURFACE itself: it uploads macros at start-up
  (52 calls in its first submission) and clears through macro 6. The macro
  processor (MME) is therefore part of M1, not a later refinement; its
  instruction set (envytools' "Fermi macro processor") is small, and every
  instruction form has a hand-assembled unit test in `tests/test_mme.c`.
- Calls arrive as a first parameter on 0xE00 + 2n and the rest on
  0xE01 + 2n; the call runs when any other method arrives or the
  submission ends.
- Keeping every method write in a per-class register file, as the hardware
  does, made each new trigger (clear, I2M launch, DMA launch) a few lines,
  and lets macros read engine state the way they expect.
- Image copies go through the DMA engine with block-linear surfaces in both
  directions; a round trip alone only proves the two directions agree, so
  the test also checks the copied image against `vi`'s decoder.

## What M3a taught

- Read the shaders before writing the interpreter. `uam -r` writes the raw
  Maxwell code and envytools' `envydis -m gm107` disassembles it (both are
  free; envytools builds in a minute, `build/envytools`). The triangle
  needed nine instructions, and the disassembler's opcode tables gave every
  field position, so nothing about the encoding was guessed.
- uam puts constant arrays in constant buffer 1 and indexes them with
  `ld c1[reg+imm]`; deko3d binds that buffer per stage with 0x8E0-0x8E2 and
  0x904 + 8 * stage.
- Fragment inputs are perspective-correct by construction: `ipa pass
  a[0x7c]` returns the interpolated 1/w, `mufu rcp` inverts it, and every
  `ipa` that follows multiplies the interpolated attr/w by it.
- Facing is decided in NDC, so the viewport's negative Y scale flips the
  window-space sign; getting that backwards culls everything silently.
- A reference renderer in a few lines of Python turned "looks right" into
  "0 of 916 168 pixels differ", and caught a bug in the checker itself (a
  PPM whose first pixel byte is 0x20 must not be split on whitespace).

## What M3b taught

- The cube needed no new instruction: a matrix transform compiles to
  `fmul`/`ffma`/`fadd` with constant-buffer operands. What it needed was
  state: uniform buffer binding 0 is constant buffer 2 for the shader (c0
  is the driver's, c1 the shader's own constants), and deko3d's
  PushConstants is LOAD_CONSTANT_BUFFER: a position at 0x8E3 and data words
  at 0x8E4 that land in the selected buffer and advance the position. Until
  that ran, every vertex came out with w = 0 and nothing was drawn.
- deko3d's debug library (`-ldeko3dd`) validated the example itself: it
  rejected a uniform buffer size that was not a multiple of 0x100, a bug in
  the test program rather than in the emulator.
- Quads are split into (0, 1, 2) and (0, 2, 3). The reference does the
  same; real hardware may choose the other diagonal, which only changes
  how colours blend inside a quad, not what is covered.

## Risks and how to keep them small

- **Shader translation is the bulk of the work.** Scope it to what
  deko3d's compiler emits for the examples first and grow with each new
  program, with every instruction covered by a unit test against
  hand-assembled inputs.
- **Unknown ioctl layouts.** Implement against what deko3d and Mesa
  actually send, logging every unknown ioctl with its size and bytes, the
  same way the `vi` parcels were worked out.
- **Performance.** A pushbuffer interpreter on the CPU is fine for M0 and
  M1; from M2 on, state changes are batched into Vulkan command buffers per
  submit rather than per method.

## References

Public, documentation-only material: the nouveau driver and envytools
(Maxwell methods and class headers), the deko3d sources (exactly what
homebrew submits), Mesa's nouveau backend, and SwitchBrew's NV services
pages for ioctl numbers.
