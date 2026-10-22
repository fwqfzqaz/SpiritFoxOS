.text
.global _start
.global EfiMain
_start:
EfiMain:
 andq $-16,%rsp
 subq $32,%rsp
 call efi_main
 cli
1: hlt
 jmp 1b
.global efi_handoff
efi_handoff:
 cli
 mov %r8,%cr3
 mov %rdx,%rdi
 jmp *%rcx
.data
.global _tls_index
_tls_index: .long 0
