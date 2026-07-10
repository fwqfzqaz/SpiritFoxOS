#ifndef _SPIRITFOXOS_SYSCALL_H
#define _SPIRITFOXOS_SYSCALL_H

#include <stdint.h>

/* SpiritFoxOS uses the Linux x86_64 syscall ABI:
 * syscall number in rax, args in rdi, rsi, rdx, r10, r8, r9
 * return value in rax */

static inline int64_t sfk_syscall0(int64_t num)
{
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t sfk_syscall1(int64_t num, int64_t a1)
{
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t sfk_syscall2(int64_t num, int64_t a1, int64_t a2)
{
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t sfk_syscall3(int64_t num, int64_t a1, int64_t a2, int64_t a3)
{
    int64_t ret;
    register int64_t r10 __asm__("r10") = a3;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t sfk_syscall4(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4)
{
    int64_t ret;
    register int64_t r10 __asm__("r10") = a3;
    register int64_t r8 __asm__("r8") = a4;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t sfk_syscall5(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5)
{
    int64_t ret;
    register int64_t r10 __asm__("r10") = a3;
    register int64_t r8 __asm__("r8") = a4;
    register int64_t r9 __asm__("r9") = a5;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t sfk_syscall6(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6)
{
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    register int64_t r8 __asm__("r8") = a5;
    register int64_t r9 __asm__("r9") = a6;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}

#endif
