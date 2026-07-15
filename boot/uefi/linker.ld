/*
 * Linker script for UEFI bootloader
 * Produces a minimal ELF that will be converted to PE32+.
 * No dynamic sections needed - fully static.
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x1000;

    .text : ALIGN(0x1000) {
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(0x1000) {
        *(.data)
        *(.data.*)
        *(.sdata)
        *(.sdata.*)
    }

    .bss : ALIGN(0x1000) {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.rela.*)
        *(.rel.*)
        *(.hash)
        *(.gnu.hash)
        *(.plt*)
        *(.got*)
    }
}
