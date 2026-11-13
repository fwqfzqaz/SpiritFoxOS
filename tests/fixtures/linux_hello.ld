/* S2 夹具的最小静态 PIE 布局：只有一个 RX 段，入口在段内。
 * 本夹具不引用任何动态重定位，因此不需要 PT_DYNAMIC。 */
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); }
SECTIONS {
 . = 0;
 .text : { *(.text .text.*) *(.rodata .rodata.*) } :text
 /DISCARD/ : { *(.comment*) *(.note*) *(.eh_frame*) *(.rela*) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) }
}
