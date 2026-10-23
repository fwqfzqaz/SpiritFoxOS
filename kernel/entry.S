.section .text.entry,"ax"
.global _start64
_start64:
 cli
 cld
 lea boot_stack_top(%rip),%rsp
 xor %rbp,%rbp
 call kernel_main
1: hlt
 jmp 1b
.section .bss
.balign 16
boot_stack: .skip 32768
boot_stack_top:
.section .text
.global arch_enter
arch_enter:
 mov %rdi,%rsp
.global restore_frame
restore_frame:
 pop %r15
 pop %r14
 pop %r13
 pop %r12
 pop %r11
 pop %r10
 pop %r9
 pop %r8
 pop %rdi
 pop %rsi
 pop %rbp
 pop %rbx
 pop %rdx
 pop %rcx
 pop %rax
 add $16,%rsp
 iretq
.global interrupt_common
interrupt_common:
 cld
 push %rax
 push %rcx
 push %rdx
 push %rbx
 push %rbp
 push %rsi
 push %rdi
 push %r8
 push %r9
 push %r10
 push %r11
 push %r12
 push %r13
 push %r14
 push %r15
 mov %rsp,%rdi
 and $-16,%rsp
 call interrupt_dispatch
 mov %rax,%rsp
 jmp restore_frame
.global load_gdt
load_gdt:
 lgdt (%rdi)
 push $8
 lea 1f(%rip),%rax
 push %rax
 lretq
1:
 mov $16,%ax
 mov %ax,%ds
 mov %ax,%es
 mov %ax,%ss
 xor %eax,%eax
 mov %ax,%fs
 mov %ax,%gs
 mov $40,%ax
 ltr %ax
 ret
