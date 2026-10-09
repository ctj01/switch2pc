# hello

The smallest HOS program: prints through `svcOutputDebugString`, sleeps one
millisecond, checks that `svcGetSystemTick` moved, raises a notification-only
`svcBreak` and calls `svcExitProcess`. Forty lines of assembly, no SDK, no MOD0.

Build (part of the normal build) and run:

```
scripts/build.sh
qemu-aarch64 -L /usr/aarch64-linux-gnu build/aarch64/src/hosrun build/aarch64/examples/hello/hello.nso
```

On native ARM64 drop the `qemu-aarch64 -L ...` prefix. Expected output:

```
[s2pc thread] I: main thread 0x100: entry 0x8000000 stack [...) tls 0x3800000000
[s2pc hosrun] I: ...hello.nso: image 0x8000000+0x1000, 7 patches (7 svc), 0 relocs (0 unresolved)
[s2pc game ] I: hello from HOS
[s2pc svc  ] I: svcBreak User [notification] addr 0 size 0
[s2pc svc  ] I: svcExitProcess
```

Exit status 0. `HOS_LOG_LEVEL=trace` shows every svc with its arguments.
