ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .multiboot : ALIGN(4) {
        *(.multiboot)
    }

    .text : ALIGN(16) {
        *(.text)
    }

    .rodata : ALIGN(16) {
        *(.rodata)
    }

    .data : ALIGN(16) {
        *(.data)
    }

    .bss : ALIGN(16) {
        *(.bss)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
