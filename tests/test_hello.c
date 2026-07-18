/*
 * SpiritFoxOS ELF 测试程序
 *
 * 静态链接的用户态程序，用于验证 ELF 加载器：
 *   - PT_LOAD 段映射
 *   - 用户态入口点跳转 (iretq)
 *   - syscall 接口 (write, getpid, exit)
 *   - BSS 段清零
 *   - 栈和 argc/argv 传递
 */

/* 直接内联 syscall，不依赖 libc */
static inline long sfk_syscall1(long num, long a1)
{
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long sfk_syscall3(long num, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory");
    return ret;
}

#define SYS_write  1
#define SYS_getpid 39
#define SYS_exit   60

/* BSS 变量 – 验证 BSS 段被正确清零 */
static int bss_test[4];

static void sys_write(int fd, const char *s, unsigned long len)
{
    sfk_syscall3(SYS_write, fd, (long)s, (long)len);
}

static void print(const char *s)
{
    unsigned long len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
}

static void print_dec(unsigned int val)
{
    char buf[12];
    int pos = 0;
    if (val == 0) {
        buf[pos++] = '0';
    } else {
        char tmp[10];
        int t = 0;
        while (val > 0) { tmp[t++] = '0' + (val % 10); val /= 10; }
        for (int i = t - 1; i >= 0; i--) buf[pos++] = tmp[i];
    }
    buf[pos] = '\0';
    print(buf);
}

int main(int argc, char **argv)
{
    /* 测试3：绕过print/strlen，直接syscall */
    sfk_syscall3(SYS_write, 1, (long)"A\n", 2);
    sfk_syscall3(SYS_write, 1, (long)"B\n", 2);
    sfk_syscall1(SYS_exit, 0);
    return 0;
}
