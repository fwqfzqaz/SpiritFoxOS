ENTRY(_sf_start)
PHDRS { text PT_LOAD FLAGS(5); ro PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); dynamic PT_DYNAMIC FLAGS(6); }
SECTIONS {
 . = 0;
 .text : ALIGN(4096) { *(.text.entry) *(.text .text.*) } :text
 . = ALIGN(4096);
 .rodata : { *(.rodata .rodata.*) } :ro
 .dynsym : { *(.dynsym) } :ro
 .dynstr : { *(.dynstr) } :ro
 .hash : { *(.hash) } :ro
 .gnu.hash : { *(.gnu.hash) } :ro
 .data : ALIGN(4096) { *(.data .data.*) *(.data.rel.ro .data.rel.ro.*) *(.got .got.*) } :data
 .dynamic : { *(.dynamic) } :data :dynamic
 .bss : ALIGN(4096) { *(.bss .bss.*) *(COMMON) } :data
 .rela.dyn : { *(.rela.dyn) } :NONE
 /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
