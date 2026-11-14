/* S3 静态 PIE 夹具布局：代码与只读数据为 RX，静态数据为独立 RW 段。 */
ENTRY(_start)
SECTIONS {
  . = 0;
  .text : { *(.text*) *(.rodata*) }
  . = ALIGN(4096);
  .data : { *(.data*) *(.bss*) *(COMMON) }
  /DISCARD/ : {
    *(.comment*) *(.note*) *(.eh_frame*) *(.eh_frame_hdr*)
    *(.rela*) *(.rel.dyn*) *(.rel.plt*) *(.got*) *(.got.plt*) *(.plt*)
    *(.dynamic*) *(.dynsym*) *(.dynstr*) *(.hash*) *(.gnu.hash*)
  }
}
