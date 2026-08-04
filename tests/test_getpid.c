/*
 * SpiritFoxOS minimal syscall test - getpid then exit
 * getpid returns a value we can verify, but doesn't do I/O
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

#define SYS_getpid 39
#define SYS_exit   60

void _start(void)
{
    /* Try a simple getpid syscall - returns pid in rax */
    long pid = sfk_syscall1(SYS_getpid, 0);

    /* If we get here without crashing, the syscall return worked.
     * Exit with the pid as code (pid should be > 0) */
    sfk_syscall1(SYS_exit, (int)pid);
    for (;;) {}
}
