# Contributing

## Build and test

`scripts/build.sh` does the full cycle on Ubuntu 24.04 (x86-64 with the ARM64
cross toolchain, or native ARM64). Every change must pass:

```
cmake --build build/host && ctest --test-dir build/host --output-on-failure
cmake --build build/aarch64 && ctest --test-dir build/aarch64 --output-on-failure
```

with zero warnings. The compiler flags are `-Wall -Wextra -Werror -Wshadow -Wvla
-Wformat=2 -Wundef`; do not weaken them, fix the code.

## Where to start

Issues labelled `compat` come from the nightly homebrew run (compat/README.md):
each names one missing service, IPC command, kernel call or GPU feature, the
apps that stop at it and where in the shims it goes. Those also labelled
`good first issue` are single commands with an example next to them.

## Adding an svc

1. Declare `svc_<name>(hos_svc_regs *r)` in `shims/hos_cpu/cpu_internal.h`.
2. Implement it in the matching `svc_*.c`. Inputs are `r->x[0..7]`, outputs go
   back into the same slots as the kernel ABI defines (result in `x0`, values in
   `x1`, `x2`). Return `HOS_KERNEL_*` codes from `src/common/hos_result.h`.
3. Point the entry in `svc_table.c` at it.
4. Add a case to `tests/test_svc.c` that calls `hos_svc_dispatch` directly.

Rules inside a shim: no `malloc`, use `mmap`; every shared structure behind a
`pthread_mutex`; never swallow an error, log it and return a result code;
remember you may be running inside a signal handler on the alternate stack.

## Adding a service

Services are classes registered by their `sm:` name; see
`shims/hos_cpu/hos_ipc.h` and `shims/hos_os/lm.c` for a complete example.

1. Write a `hos_service_class` with a dispatch function. It receives the
   command id, the request payload (`ctx->in`), buffers (`ctx->a`, `ctx->b`,
   `ctx->x`, `ctx->c`, guest addresses that are plain host pointers) and
   writes its reply with `hos_ipc_out_write`. Return the HOS result for the
   SFCO header. Interfaces returned to the game go through
   `hos_ipc_return_object`; the transport turns them into object ids on domain
   sessions and move handles otherwise.
2. Register it from a library constructor with `hos_ipc_register_service`.
3. Add a case to `tests/test_ipc.c` using the header-only client in
   `examples/kernel_test/ipc_client.h`, and ideally a check in
   `examples/kernel_test/main.c` so it also runs from inside a real module.

Service libraries live in `shims/hos_<name>/` with their own `CMakeLists.txt`
producing `libhos_<name>.so`, linked into hosrun with `--no-as-needed` so they
stay in `DT_NEEDED`. Shims do not `dlopen` each other.

## Style

`clang-format -i` with the repository `.clang-format`. Comments say why, not
what. A `FIXME` without an issue number from `docs/ISSUES.md` does not merge.

## Scope

Pull requests that add decryption, key handling or download code will be
closed. See `docs/design.md`, section 1.
