/*
 * SpiritFoxOS 自动运行模块
 *
 * 在启动时读取并执行配置文件中的命令。
 * 默认文件：/etc/autorun.cfg
 *
 * 文件格式：
 *   - 每行一条命令
 *   - 以 '#' 开头的行是注释
 *   - 空行将被跳过
 */

#include "autorun.h"
#include "vfs.h"
#include "shell.h"
#include "string.h"
#include "vga.h"
#include "serial.h"

#define AUTORUN_MAX_LINE  256
#define AUTORUN_MAX_ARGS  16

int autorun_execute(const char *path)
{
    if (!path)
        path = AUTORUN_DEFAULT_PATH;

    int fd = vfs_open(path, VFS_O_RDONLY, 0);
    if (fd < 0) {
        serial_puts("[autorun] file not found: ");
        serial_puts(path);
        serial_puts("\n");
        return -1;
    }

    printf("[autorun] executing %s\n", path);
    serial_puts("[autorun] executing ");
    serial_puts(path);
    serial_puts("\n");

    char line[AUTORUN_MAX_LINE];
    int line_pos = 0;
    char buf[128];
    int n;
    int cmd_count = 0;

    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[line_pos] = '\0';

                /* 去除行尾回车符（Windows 换行符） */
                if (line_pos > 0 && line[line_pos - 1] == '\r')
                    line[--line_pos] = '\0';

                /* 跳过前导空白 */
                char *cmd = line;
                while (*cmd == ' ' || *cmd == '\t')
                    cmd++;

                /* 跳过空行和注释 */
                if (*cmd != '\0' && *cmd != '#') {
                    printf("[autorun] > %s\n", cmd);

                    /* 通过 shell 解析并执行 */
                    char *argv[AUTORUN_MAX_ARGS];
                    int argc = 0;
                    char *token = strtok(cmd, " \t");
                    while (token != NULL && argc < AUTORUN_MAX_ARGS) {
                        argv[argc++] = token;
                        token = strtok(NULL, " \t");
                    }

                    if (argc > 0) {
                        shell_execute(argv[0], argc, argv);
                        cmd_count++;
                    }
                }

                line_pos = 0;
            } else {
                if (line_pos < AUTORUN_MAX_LINE - 1)
                    line[line_pos++] = buf[i];
            }
        }
    }

    /* 处理没有尾部换行符的最后一行 */
    if (line_pos > 0) {
        line[line_pos] = '\0';
        if (line_pos > 0 && line[line_pos - 1] == '\r')
            line[--line_pos] = '\0';

        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t')
            cmd++;

        if (*cmd != '\0' && *cmd != '#') {
            printf("[autorun] > %s\n", cmd);

            char *argv[AUTORUN_MAX_ARGS];
            int argc = 0;
            char *token = strtok(cmd, " \t");
            while (token != NULL && argc < AUTORUN_MAX_ARGS) {
                argv[argc++] = token;
                token = strtok(NULL, " \t");
            }

            if (argc > 0) {
                shell_execute(argv[0], argc, argv);
                cmd_count++;
            }
        }
    }

    vfs_close(fd);

    printf("[autorun] %d command(s) executed\n", cmd_count);
    serial_puts("[autorun] done, executed ");
    serial_put_dec((uint64_t)cmd_count);
    serial_puts(" commands\n");

    return cmd_count;
}

int autorun_create_default(const char *path)
{
    if (!path)
        path = AUTORUN_DEFAULT_PATH;

    /* 检查文件是否已存在 */
    int fd = vfs_open(path, VFS_O_RDONLY, 0);
    if (fd >= 0) {
        vfs_close(fd);
        return 0; /* 文件已存在，不覆盖 */
    }

    /* 创建默认的 autorun.cfg */
    fd = vfs_open(path, VFS_O_CREAT | VFS_O_WRONLY,
                  VFS_S_IRUSR | VFS_S_IWUSR);
    if (fd < 0)
        return -1;

    const char *default_content =
        "# SpiritFoxOS Autorun Configuration\n"
        "#\n";

    vfs_write(fd, default_content, strlen(default_content));
    vfs_close(fd);

    printf("[autorun] created default %s\n", path);
    return 0;
}
