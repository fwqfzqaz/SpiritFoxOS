/* musl libc 静态链接测试程序
 * 编译: x86_64-spiritfoxos-gcc -static -o test_musl_hello test_musl_hello.c
 * 验证 musl libc 在 SpiritFoxOS 上的基本功能
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    printf("Hello, SpiritFoxOS from musl libc!\n");

    /* 测试 getpid */
    pid_t pid = getpid();
    printf("PID: %d\n", pid);

    /* 测试字符串函数 */
    char buf[64];
    strcpy(buf, "musl");
    strcat(buf, " libc");
    printf("String: %s (len=%zu)\n", buf, strlen(buf));

    /* 测试内存分配 */
    char *p = malloc(128);
    if (p) {
        memset(p, 'A', 128);
        p[127] = '\0';
        printf("Malloc OK, first char: %c\n", p[0]);
        free(p);
    }

    /* 测试 argc/argv */
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc && i < 3; i++)
        printf("argv[%d]=%s\n", i, argv[i]);

    return 0;
}
