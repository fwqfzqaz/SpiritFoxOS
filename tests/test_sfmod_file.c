/* 用户态功能检查：Linux 文件调用经过 VFS 兼容桥进入模块。 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void)
{
    const char *path = "/modules/user-test.txt";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    char buffer[16];
    if (fd < 0) goto fail;
    if (write(fd, "abcdef", 6) != 6 || ftruncate(fd, 2) || ftruncate(fd, 6) ||
        lseek(fd, 0, SEEK_SET) != 0 || read(fd, buffer, sizeof(buffer)) != 6 ||
        buffer[0] != 'a' || buffer[1] != 'b' || buffer[2] || buffer[5]) goto fail;
    if (ftruncate(fd, 65537) == 0 || close(fd) || unlink(path)) goto fail;
    if (open(path, O_RDONLY) >= 0) goto fail;
    fd = open("/modules/README", O_RDONLY);
    if (fd < 0 || read(fd, buffer, 4) != 4 || memcmp(buffer, "This", 4) || close(fd))
        goto fail;
    for (int i = 0; i < 16; i++) {
        pid_t child = fork();
        if (!child) _exit(0);
        int status = -1;
        if (child < 0 || waitpid(child, &status, 0) != child || status) goto fail;
    }
    puts("[sfmod-user] file IO passed");
    puts("[sfmod-user] fork/wait stress passed (16 children)");
    return 0;
fail:
    puts("[sfmod-user] FAILED");
    return 1;
}
