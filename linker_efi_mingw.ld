/* ============================================================
 * SpiritFoxOS Linker Script - UEFI Mode (mingw-w64/PE)
 *
 * Used with x86_64-w64-mingw32-gcc which produces native
 * PE/COFF executables. This script defines section layout.
 *
 * Key difference from linker_efi.ld: sections start at 0x1000
 * to leave room for PE headers (required by UEFI firmware).
 * ============================================================ */

OUTPUT_FORMAT("pe-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_main)

SECTIONS {
    /* First page reserved for DOS/PE/COFF headers */
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
     * Contains base relocation entries for ASLR.
     * With -mcmodel=large and mingw, the linker generates
     * actual relocations here automatically.
     */
    .reloc : {
        /* Dummy reloc block as fallback if linker adds none */
        LONG(0);      /* Page RVA */
        LONG(12);     /* Block size */
        SHORT(0); SHORT(0); SHORT(0); SHORT(0);
    }

    /* Kernel bounds symbols */
    _start = 0x1000;
    _end = .;

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.eh_frame*)
        *(.eh_frame_hdr*)
    }
}
