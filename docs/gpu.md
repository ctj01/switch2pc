# GPU plan (#40)

Status: plan, nothing implemented. Today `nvdrv` serves `nvmap` and
`nvhost-ctrl`, `vi` composites CPU-drawn framebuffers, and any command
submission to the GPU fails. Everything that runs (libnx console, DOOM,
Quake, SDL2 games) draws with the CPU.

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

1. **M0, the driver accepts the GPU (weeks).** `nvhost-ctrl-gpu`
   characteristics and zcull queries, `nvhost-as-gpu` address space and
   mappings, `nvhost-gpu` channel setup and `SUBMIT_GPFIFO` with fences
   that signal at once, `nvhost-ctrl` syncpoint waits. Commands are parsed
   and logged but not executed. Exit: a deko3d program creates a device,
   queue and swapchain, submits frames and exits cleanly.
2. **M1, clears and copies (weeks).** 3D engine clears into render
   targets, the DMA copy engine (linear and block-linear), inline-to-memory
   uploads, semaphore releases. Executed on the CPU first, which needs the
   block-linear swizzle encoder next to today's decoder. Exit: deko3d's
   clear-screen example shows its colour through `vi`.
3. **M2, a Vulkan backend for what M1 does (weeks).** Same results, on the
   host GPU. This is where window and GPU meet; the X11 window gets a
   Vulkan swapchain.
4. **M3, shaders (months).** Decode Maxwell SASS for vertex and fragment
   stages (the instructions deko3d's compiler emits for simple GLSL),
   translate to SPIR-V, vertex fetch, constant buffers, rasterisation
   state. Exit: deko3d's triangle and cube examples.
5. **M4, textures (weeks).** TIC/TSC decoding, texture formats, samplers,
   swizzled texture upload. Exit: a textured quad; then SDL2 through Mesa.
6. **M5, the rest (open-ended).** Compute, queries, transform feedback,
   tessellation and geometry stages, as real homebrew needs them.

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
