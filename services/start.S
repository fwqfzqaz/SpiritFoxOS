.section .text.entry,"ax"
.global _sf_start
_sf_start:
 xor %ebp,%ebp
 sub $8,%rsp
 call sf_main
 mov %rax,%rdi
 xor %eax,%eax
 int $0x80
1: ud2
 jmp 1b
