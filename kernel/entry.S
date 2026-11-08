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
/* 进入每 CPU 空闲栈：当前内核栈仍属于刚被换出的任务，另一 CPU 可能马上
 * 调度该任务并复用同一 kstack；必须先切到独立空闲栈，再切换到内核地址
 * 空间（任务页表可能随后被回收，LAPIC 必须始终可写），清除该任务的
 * cpu 归属，最后释放全局锁并进入 hlt 循环。 */
.global enter_idle
enter_idle:
 cli
 mov %rdi,%rsp
 mov %rdx,%cr3
 test %rsi,%rsi
 jz 1f
 movl $-1,(%rsi)
1:
 lock incl kernel_lock+4(%rip)
 jmp idle_loop
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
