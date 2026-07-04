ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .multiboot : ALIGN(4) {
        *(.multiboot)
    }

    .text : ALIGN(16) {
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(16) {
        *(.data)
        *(.data.*)
    }

    .bss : ALIGN(16) {
        *(.bss)
        *(COMMON)
    }

    /* 64-bit kernel data loaded at 0x102000 */
    .kernel 0x102000 : ALIGN(16) {
        *(.kernel)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
