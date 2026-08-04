/*
 * SpiritFoxOS minimal syscall test - getpid then exit
 * Uses _start and inline syscalls, no libc
 */
#define SYS_getpid 39
#define SYS_exit   60

void _start(void)
{
    long ret;
    /* getpid syscall */
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"((long)SYS_getpid), "D"(0L)
        : "rcx", "r11", "memory");

    /* exit with 0 */
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"((long)SYS_exit), "D"(0L)
        : "rcx", "r11", "memory");

    for (;;) {}
}
