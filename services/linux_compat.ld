/* S2 兼容 stub/probe 的固定布局：代码从 0 开始，数据页固定在 0x1000。
 * loader 按 RX/RW 两段追加，stub 用 RIP 自定位，因此无需重定位。 */
ENTRY(_sf_linux_stub_start)
SECTIONS {
  . = 0;
  .text : { *(.text*) }
  . = 0x1000;
  .data : { *(.data*) *(.bss*) }
  /DISCARD/ : { *(.comment*) *(.note*) *(.eh_frame*) *(.rela*) *(.rodata*) }
}
