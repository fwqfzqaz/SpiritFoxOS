/* ============================================================
 * SpiritFoxOS 链接器脚本 - UEFI模式
 * 简单布局；PE/COFF头由objcopy添加。
 *
 * 本脚本生成具有明确定义节的干净ELF。
 * Makefile使用 objcopy --target=pei-x86-64 转换为
 * PE格式，这会自动生成UEFI固件所需的
 * 有效COFF节头。
 * ============================================================ */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(efi_main)

SECTIONS {
    /*
     * UEFI PE镜像要求节虚拟地址从
     * SectionAlignment（0x1000）开始。第一页保留给
     * PE/DOS/COFF头。从VA=0开始会导致头和代码
     * 重叠，UEFI固件会将其拒绝为"Unsupported"。
     */
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
     * UEFI加载器拒绝没有.reloc节的镜像。
     * 我们使用-mcmodel=large，所以实际上不需要重定位，
     * 但我们必须包含至少一个虚拟条目。
     * 格式：页面RVA（4字节）+ 块大小（4字节）+ 条目...
     * 类型 IMAGE_REL_BASED_ABSOLUTE (0) = 填充（每个2字节）。
     */
    .reloc : {
        /* 页面RVA = 0（镜像基址） */
        LONG(0);
        /* 块大小 = 12（4字节头 + 8字节用于4个ABSOLUTE条目） */
        LONG(12);
        /* 4个IMAGE_REL_BASED_ABSOLUTE（每个2字节）= 8字节 */
        SHORT(0); SHORT(0); SHORT(0); SHORT(0);
    }

    /* 内核边界符号 */
    _start = 0;
    _end = .;

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.eh_frame*)
        *(.eh_frame_hdr*)
    }
}
