/*
 * crt0 for a library module (sdk, subsdk*): MOD0 so the linker can find the
 * dynamic section, and a start that only returns. Nothing ever branches here;
 * the module is used through its exported symbols.
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
    .word 0
    .word 0
    .word 0
1:
    ret
    .size _start, . - _start

    .section .note.GNU-stack, "", %progbits
