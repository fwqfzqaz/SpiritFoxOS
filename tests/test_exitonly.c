/*
 * SpiritFoxOS minimal syscall test - only exit
 */

static inline long sfk_syscall1(long num, long a1)
{
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

#define SYS_exit   60

void _start(void)
{
    sfk_syscall1(SYS_exit, 42);
    for (;;) {}
}
