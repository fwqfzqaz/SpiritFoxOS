# SpiritFoxOS libc - crt1.o
# Entry point for user programs

.section .text
.global _start
.type _start, @function

_start:
    xor %ebp, %ebp          # Clear frame pointer
    mov (%rsp), %rdi         # argc
    lea 8(%rsp), %rsi        # argv
    lea 16(%rsp,%rdi,8), %rdx  # envp = &argv[argc+1]
    
    # Align stack to 16 bytes
    and $-16, %rsp
    
    call _start_c
    
    # Should not return, but just in case
    mov $60, %rax            # sys_exit
    xor %edi, %edi
    syscall
