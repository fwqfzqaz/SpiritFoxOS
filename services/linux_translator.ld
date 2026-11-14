/* S3 翻译器布局：代码与只读数据从 0 开始（RX），静态数据固定在 0x10000（RW）。
 * 两段相对距离固定，故所有 RIP 相对引用无需重定位；构建期校验 .rela* 为空。 */
ENTRY(_sf_linux_translator_start)
SECTIONS {
  . = 0;
  .text : { *(.text.start) *(.text*) *(.rodata*) }
  . = 0x10000;
  .data : {
    __sf_linux_translator_data = .;
    . += 16; /* 首 16 字节保留给 sf-init 写入入口地址与入口写入的栈顶。 */
    *(.data*) *(.bss*) *(COMMON)
  }
  /DISCARD/ : {
    *(.comment*) *(.note*) *(.eh_frame*) *(.eh_frame_hdr*)
    *(.rela*) *(.rel.dyn*) *(.rel.plt*) *(.got*) *(.got.plt*) *(.plt*)
    *(.dynamic*) *(.dynsym*) *(.dynstr*) *(.hash*) *(.gnu.hash*)
  }
}
