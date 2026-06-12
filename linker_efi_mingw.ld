/* ============================================================
 * SpiritFoxOS 链接器脚本 - UEFI模式（mingw-w64/PE）
 *
 * 与 x86_64-w64-mingw32-gcc 配合使用，该编译器生成原生
 * PE/COFF可执行文件。本脚本定义节布局。
 *
 * 与linker_efi.ld的主要区别：节从0x1000开始
 * 以留出PE头空间（UEFI固件所必需）。
 * ============================================================ */

OUTPUT_FORMAT("pe-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_main)

SECTIONS {
    /* 第一页保留给DOS/PE/COFF头 */
    . = 0x1000;

    /* 代码节 */
    .text : {
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
    }

    /* 只读数据 */
    .rodata : {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
    }

    /* 已初始化数据 */
    .data : {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
    }

    /* BSS（未初始化数据） */
    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        __bss_end = .;
    }

    /*
     * 重定位节 - UEFI固件所必需！
     * 包含用于ASLR的基址重定位条目。
     * 使用-mcmodel=large和mingw时，链接器会在此处
     * 自动生成实际的重定位。
     */
    .reloc : {
        /* 如果链接器未添加任何内容，则使用虚拟重定位块作为回退 */
        LONG(0);      /* 页面RVA */
        LONG(12);     /* 块大小 */
        SHORT(0); SHORT(0); SHORT(0); SHORT(0);
    }

    /* 内核边界符号 */
    _start = 0x1000;
    _end = .;

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.eh_frame*)
        *(.eh_frame_hdr*)
    }
}
