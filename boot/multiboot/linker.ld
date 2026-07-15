/* Linker script for 32-bit multiboot loader */
ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    .multiboot : ALIGN(4) {
        *(.multiboot)
    }

    .text : {
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
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
