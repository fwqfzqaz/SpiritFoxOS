/*
 * EFI Stub 链接脚本
 * 生成最小 ELF，随后由 objcopy 转为 PE32+。
 * 保留 .rela 段供 gen_reloc.py 生成 PE 重定位表。
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

    /* 保留重定位段，供 gen_reloc.py 提取 */
    .rela.dyn : {
        *(.rela.init)
        *(.rela.text .rela.text.*)
        *(.rela.rodata .rela.rodata.*)
        *(.rela.data .rela.data.*)
        *(.rela.sdata .rela.sdata.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.gnu.hash)
        *(.plt*)
        *(.got*)
    }
}
