/*
 * crt0 for the homebrew ABI test module. The first 0x80 bytes are left for the
 * NRO header that tools/nso2nro.py writes in place (a branch over it and the
 * MOD0 offset stay the module's own), as libnx's crt0 does for elf2nro. On
 * entry x0 holds the config entries and x1 is -1 (an NSO entry has x0 = 0 and
 * the main thread handle in x1); main returns here and we return through lr,
 * the address the loader handed us, which exits the process.
 */

    .section .crt0, "ax", %progbits
    .globl _start
    .type _start, %function
_start:
    b 1f
    .word __mod0 - _start
    .fill 0x78, 1, 0            /* the NRO header, written by nso2nro */
__mod0:
    .ascii "MOD0"
    .word _DYNAMIC - __mod0
    .word __bss_start - __mod0
    .word __bss_end - __mod0
    .word 0                     /* eh_frame_hdr start */
    .word 0                     /* eh_frame_hdr end */
    .word 0                     /* runtime module object */
1:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl hbabi_main
    ldp x29, x30, [sp], #16
    ret                         /* back to the loader's stub: svcExitProcess */
    .size _start, . - _start

    .section .note.GNU-stack, "", %progbits
