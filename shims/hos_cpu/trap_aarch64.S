/*
 * Entry and trap glue for libhos_cpu. ARM64 only.
 *
 * hos_enter: switch the calling thread into game context. Loads the HOS TLS
 * block into tpidr_el0, moves the stack pointer and branches. Never returns.
 *
 * hos_trap_entry: the sigaction handler for SIGTRAP, SIGSEGV, SIGBUS, SIGILL
 * and SIGFPE. If tpidr_el0 currently holds a block inside the HOS TLS arena
 * we are in game context: swap the host TCB back in before touching any C
 * code, then swap the game block back on the way out. The kernel does not
 * save tpidr_el0 in the signal frame, so this must be done by hand.
 */

#include "tls_layout.h"

    .text
    .align 2

    .globl hos_enter
    .hidden hos_enter
    .type hos_enter, %function
hos_enter:
    /* x0 = entry, x1 = game x0, x2 = game x1, x3 = sp, x4 = tls, x5 = lr */
    msr tpidr_el0, x4
    mov sp, x3
    mov x16, x0
    mov x0, x1
    mov x1, x2
    mov x2, xzr
    mov x3, xzr
    mov x4, xzr
    mov x29, xzr
    mov x30, x5
    br x16
    .size hos_enter, . - hos_enter

    .globl hos_trap_entry
    .hidden hos_trap_entry
    .type hos_trap_entry, %function
hos_trap_entry:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]

    mrs x19, tpidr_el0
    adrp x20, hos_tls_arena_base
    ldr x20, [x20, :lo12:hos_tls_arena_base]
    adrp x21, hos_tls_arena_size
    ldr x21, [x21, :lo12:hos_tls_arena_size]
    sub x22, x19, x20
    cmp x22, x21
    b.hs 1f
    /* game context: restore the host TCB */
    ldr x22, [x19, #HOS_TLS_HOST_TP]
    msr tpidr_el0, x22
    mov x3, #1
    b 2f
1:
    mov x3, #0
2:
    /* x0 = sig, x1 = siginfo, x2 = ucontext, x3 = in_game */
    bl hos_trap_c

    sub x22, x19, x20
    cmp x22, x21
    b.hs 3f
    msr tpidr_el0, x19
3:
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #48
    ret
    .size hos_trap_entry, . - hos_trap_entry

    .section .note.GNU-stack, "", %progbits
