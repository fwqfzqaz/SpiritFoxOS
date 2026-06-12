ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(5);  /* R+X */
    rodata PT_LOAD FLAGS(4);  /* R   */
    data   PT_LOAD FLAGS(6);  /* R+W */
}

SECTIONS {
    . = 1M;

    .text ALIGN(4K) : {
        *(.multiboot2_header)
        _start = .;           /* 头之后的入口点 */
        *(.text .text.*)
    } :text

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    } :rodata

    .data ALIGN(4K) : {
        *(.data .data.*)
    } :data

    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    _end = .;

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
    }
}
