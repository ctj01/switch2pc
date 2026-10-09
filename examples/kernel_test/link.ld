/*
 * Static-PIE layout for test modules: three page-aligned PT_LOAD segments in
 * the order elf2nso expects (text, rodata, data+bss). The dynamic section and
 * GOT live in the writable segment so the loader can relocate them.
 */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
    dyn    PT_DYNAMIC;
}

SECTIONS {
    . = 0;

    .text : {
        KEEP(*(.crt0))
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    .rela.dyn : { *(.rela.*) } :rodata
    .hash     : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .dynsym   : { *(.dynsym) } :rodata
    .dynstr   : { *(.dynstr) } :rodata

    . = ALIGN(0x1000);
    .data : {
        *(.data .data.*)
    } :data
    .dynamic : { *(.dynamic) } :data :dyn
    .got : { *(.got) *(.got.plt) } :data
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :data
    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    /DISCARD/ : {
        *(.interp)
        *(.note.*)
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.gnu.version*)
    }
}
