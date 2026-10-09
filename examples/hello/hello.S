/*
 * hello: the smallest HOS program. Prints through svcOutputDebugString,
 * sleeps one millisecond, checks that the system tick advanced, raises a
 * notification-only svcBreak and exits. No MOD0, no relocations, no SDK.
 *
 * Build: see examples/CMakeLists.txt. Run: hosrun hello.nso
 */

    .text
    .globl _start
    .type _start, %function
_start:
    b 1f
    .word 0                     /* word at +4: MOD0 offset; 0 = none */
1:
    adr x0, msg
    mov x1, #(msg_end - msg)
    svc #0x27                   /* OutputDebugString(msg, len) */

    movz x0, #0x4240
    movk x0, #0xF, lsl #16      /* 1 000 000 ns */
    svc #0x0B                   /* SleepThread(ns) */

    svc #0x1E                   /* GetSystemTick -> x0 */
    mov x19, x0

    movz x0, #0x0002
    movk x0, #0x8000, lsl #16   /* BreakReason User | NotificationOnly */
    mov x1, xzr
    mov x2, xzr
    svc #0x26                   /* Break(reason, 0, 0): logged, execution continues */

    svc #0x1E
    cmp x0, x19
    b.hi 2f

    mov x0, #0                  /* BreakReason Panic: the tick did not move */
    mov x1, xzr
    mov x2, xzr
    svc #0x26
2:
    svc #0x07                   /* ExitProcess */
    b .
    .size _start, . - _start

msg:
    .ascii "hello from HOS\n"
msg_end:

    .section .note.GNU-stack, "", %progbits
