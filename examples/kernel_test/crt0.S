/*
 * crt0 for freestanding test modules. The first eight bytes are what nnrtld
 * expects: a branch and the MOD0 offset. Relocations are applied by the
 * loader before we run, so nothing is done here but calling main.
 */

    .section .crt0, "ax", %progbits
    .globl _start
    .type _start, %function
_start:
    b 1f
    .word __mod0 - _start
__mod0:
    .ascii "MOD0"
    .word _DYNAMIC - __mod0
    .word __bss_start - __mod0
    .word __bss_end - __mod0
    .word 0                     /* eh_frame_hdr start */
    .word 0                     /* eh_frame_hdr end */
    .word 0                     /* runtime module object */
1:
    /* x0 = exception info (0), x1 = main thread handle */
    mov x0, x1
    bl hos_main
    svc #0x07                   /* ExitProcess, in case main returns */
    b .
    .size _start, . - _start

    .section .note.GNU-stack, "", %progbits
