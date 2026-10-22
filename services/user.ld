ENTRY(_sf_start)
PHDRS { text PT_LOAD FLAGS(5); ro PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x40000000;
 .text : ALIGN(4096) { *(.text.entry) *(.text .text.*) } :text
 .rodata : ALIGN(4096) { *(.rodata .rodata.*) } :ro
 .data : ALIGN(4096) { *(.data .data.*) } :data
 .bss : ALIGN(4096) { *(.bss .bss.*) *(COMMON) } :data
 /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
