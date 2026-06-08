/* ============================================================
 * SpiritFoxOS Linker Script - UEFI Mode
 * Simple layout; PE/COFF headers are added by objcopy.
 *
 * This script produces a clean ELF with well-defined sections.
 * The Makefile uses objcopy --target=pei-x86-64 to convert
 * to PE format, which automatically generates valid COFF
 * section headers required by UEFI firmware.
 * ============================================================ */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(efi_main)

SECTIONS {
    /*
     * UEFI PE images require section VirtualAddresses to start at
     * SectionAlignment (0x1000). The first page is reserved for
     * PE/DOS/COFF headers. Starting at VA=0 causes headers and
     * code to overlap, which UEFI firmware rejects as "Unsupported".
     */
    . = 0x1000;

    /* Code section */
    .text : {
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
    }

    /* Read-only data */
    .rodata : {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
    }

    /* Initialized data */
    .data : {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
    }

    /* BSS (uninitialized data) */
    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        __bss_end = .;
    }

    /*
     * Relocation section - REQUIRED by UEFI firmware!
     * UEFI loader refuses to load images without a .reloc section.
     * We use -mcmodel=large so actual relocations are not needed,
     * but we must include at least one dummy entry.
     * Format: PAGE RVA (4 bytes) + BLOCK SIZE (4 bytes) + entries...
     * Type IMAGE_REL_BASED_ABSOLUTE (0) = padding (2 bytes each).
     */
    .reloc : {
        /* Page RVA = 0 (base of image) */
        LONG(0);
        /* Block size = 12 (4 header + 8 for 4 ABSOLUTE entries) */
        LONG(12);
        /* 4 x IMAGE_REL_BASED_ABSOLUTE (2 bytes each) = 8 bytes */
        SHORT(0); SHORT(0); SHORT(0); SHORT(0);
    }

    /* Kernel bounds symbols */
    _start = 0;
    _end = .;

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.eh_frame*)
        *(.eh_frame_hdr*)
    }
}
