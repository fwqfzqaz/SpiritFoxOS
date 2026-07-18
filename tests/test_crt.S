/* SpiritFoxOS ELF 测试程序入口点 */

.section .text
.global _start
.type _start, @function

_start:
    xor %ebp, %ebp
    mov (%rsp), %rdi         # argc
    lea 8(%rsp), %rsi        # argv
    lea 16(%rsp,%rdi,8), %rdx  # envp

    and $-16, %rsp           # 对齐栈

    call main

    # exit(ret)
    mov $60, %rax
    mov %eax, %edi
    syscall
