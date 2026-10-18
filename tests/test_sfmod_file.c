/* 用户态功能检查：Linux 文件调用经过 VFS 兼容桥进入模块。 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>

#define VERIFY(expr) do { if (!(expr)) { printf("[sfmod-user] FAILED line %d\n", __LINE__); return -1; } } while (0)

static int special_devices(void)
{
    char data[8192];
    memset(data, 0x5a, sizeof(data));
    int null = open("/dev/null", O_RDWR);
    int zero = open("/dev/zero", O_RDWR);
    struct stat status;
    VERIFY(null >= 0 && zero >= 0);
    VERIFY(!fstat(null, &status) && S_ISCHR(status.st_mode) &&
           major(status.st_rdev) == 1 && minor(status.st_rdev) == 3 && !status.st_size);
    VERIFY(!fstat(zero, &status) && S_ISCHR(status.st_mode) &&
           major(status.st_rdev) == 1 && minor(status.st_rdev) == 5);
    VERIFY(read(null, data, sizeof(data)) == 0 && write(null, data, sizeof(data)) == 4096);
    VERIFY(read(zero, data, sizeof(data)) == 4096);
    for (int i = 0; i < 4096; i++) VERIFY(!data[i]);
    VERIFY(write(zero, data, 7) == 7 && lseek(null, 100, SEEK_SET) == 0 &&
           lseek(zero, -100, SEEK_CUR) == 0 && !fstat(zero, &status) && !status.st_size);
    VERIFY(!read(zero, NULL, 0) && !write(null, NULL, 0));
    for (int target = 1; target <= 2; target++) {
        int saved = dup(target);
        VERIFY(saved >= 0 && dup2(null, target) == target);
        const char marker[] = "[sfmod-user] redirect leaked\n";
        ssize_t written = write(target, marker, sizeof(marker) - 1);
        struct iovec vector = {(void *)marker, sizeof(marker) - 1};
        ssize_t vectored = writev(target, &vector, 1);
        int restored = dup2(saved, target);
        int closed = close(saved);
        VERIFY(restored == target && !closed && written == sizeof(marker) - 1 &&
               vectored == sizeof(marker) - 1);
    }
    VERIFY(ftruncate(zero, 0) < 0 && ioctl(null, 0x1234, 0) < 0 && errno == ENOTTY);
    VERIFY(mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, zero, 0) == MAP_FAILED && errno == ENODEV);
    VERIFY(mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, null, 0) == MAP_FAILED && errno == ENODEV);
    int shared = dup(zero);
    VERIFY(shared >= 0 && !close(zero) && read(shared, data, 8) == 8 && !close(shared));
    VERIFY(unlink("/dev/null") < 0 && unlink("/dev/zero") < 0 && !close(null));
    null = open("/dev/null", O_RDONLY);
    VERIFY(null >= 0 && write(null, data, 1) < 0 && !close(null));
    zero = open("/dev/zero", O_WRONLY);
    VERIFY(zero >= 0 && read(zero, data, 1) < 0 && !close(zero));
    DIR *directory = opendir("/dev");
    VERIFY(directory);
    int seen_null = 0, seen_zero = 0, seen_input = 0, seen_dri = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, "null")) seen_null++;
        if (!strcmp(entry->d_name, "zero")) seen_zero++;
        if (!strcmp(entry->d_name, "input")) seen_input++;
        if (!strcmp(entry->d_name, "dri")) seen_dri++;
    }
    VERIFY(!closedir(directory) && seen_null == 1 && seen_zero == 1 &&
           seen_input == 1 && seen_dri == 1);
    puts("[sfmod-user] special devices passed");
    return 0;
}

static int tree_test(void)
{
    const char *path = "/tmp/sfmod-user/sub/data";
    VERIFY(!mkdir("/tmp/sfmod-user", 0700) && !mkdir("/tmp/sfmod-user/sub", 0700));
    VERIFY(!mkdir("/run/sfmod-user", 0700));
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    VERIFY(fd >= 0 && write(fd, "old", 3) == 3);
    VERIFY(rmdir("/tmp/sfmod-user/sub") < 0 && rmdir("/tmp") < 0);
    DIR *directory = opendir("/tmp/sfmod-user/sub");
    VERIFY(directory);
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (!strcmp(entry->d_name, "data") && entry->d_type == DT_REG) found++;
    VERIFY(found == 1 && !closedir(directory));
    int duplicate = dup(fd);
    VERIFY(duplicate >= 0 && !unlink(path));
    pid_t reader = fork();
    if (!reader) _exit(0); /* 退出必须只释放子进程对共享描述的引用。 */
    int child_status = -1;
    VERIFY(reader > 0 && waitpid(reader, &child_status, 0) == reader && !child_status);
    struct stat status;
    VERIFY(!fstat(fd, &status) && status.st_nlink == 0);
    int replacement = open(path, O_CREAT | O_RDWR, 0600);
    VERIFY(replacement >= 0 && write(replacement, "new", 3) == 3);
    char buffer[8];
    VERIFY(lseek(fd, 0, SEEK_SET) == 0 && read(duplicate, buffer, sizeof(buffer)) == 3 &&
           !memcmp(buffer, "old", 3));
    VERIFY(!close(fd) && lseek(duplicate, 0, SEEK_SET) == 0 &&
           read(duplicate, buffer, 3) == 3 && !memcmp(buffer, "old", 3) && !close(duplicate));
    int independent = open("/run/sfmod-user/data", O_CREAT | O_RDWR, 0600);
    VERIFY(independent >= 0 && write(independent, "run", 3) == 3 && !close(independent));
    int append = open(path, O_WRONLY | O_APPEND);
    VERIFY(append >= 0 && lseek(append, 0, SEEK_SET) == 0 && write(append, "+", 1) == 1 && !close(append));
    VERIFY(lseek(replacement, 0, SEEK_SET) == 0 && read(replacement, buffer, 8) == 4 &&
           !memcmp(buffer, "new+", 4) && !close(replacement));
    fd = open(path, O_WRONLY | O_TRUNC);
    VERIFY(fd >= 0 && !fstat(fd, &status) && status.st_size == 0 && !close(fd));
    fd = open(path, O_RDONLY);
    VERIFY(fd >= 0 && write(fd, "x", 1) < 0 && read(fd, buffer, 8) == 0 && !close(fd));
    independent = open("/run/sfmod-user/data", O_RDONLY);
    VERIFY(independent >= 0 && read(independent, buffer, 8) == 3 &&
           !memcmp(buffer, "run", 3) && !close(independent));
    VERIFY(!unlink(path) && !rmdir("/tmp/sfmod-user/sub") && !rmdir("/tmp/sfmod-user"));
    VERIFY(!unlink("/run/sfmod-user/data") && !rmdir("/run/sfmod-user"));
    /* 超过节点上限次数的创建/删除，确认最后 close 实际回收模块节点。 */
    for (int i = 0; i < 80; i++) {
        fd = open("/tmp/recycle", O_CREAT | O_RDWR, 0600);
        VERIFY(fd >= 0 && write(fd, "x", 1) == 1 && !unlink("/tmp/recycle") && !close(fd));
    }
    char boundary[80] = "/tmp/";
    memset(boundary + 5, 'n', 63);
    boundary[68] = 0;
    fd = open(boundary, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    VERIFY(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC) && !close(fd));
    boundary[68] = 'n'; boundary[69] = 0;
    VERIFY(open(boundary, O_CREAT | O_RDWR, 0600) < 0);
    boundary[68] = 0;
    VERIFY(!unlink(boundary));
    /* /tmp 和 /run 保留旧 memfs 的 1 MiB 单文件容量，而非演示区的 64 KiB。 */
    const char *large_paths[] = {"/tmp/large-temp", "/run/large-temp"};
    for (int i = 0; i < 2; i++) {
        fd = open(large_paths[i], O_CREAT | O_RDWR, 0600);
        VERIFY(fd >= 0 && lseek(fd, 70000, SEEK_SET) == 70000 && write(fd, "big", 3) == 3);
        VERIFY(lseek(fd, 69998, SEEK_SET) == 69998 && read(fd, buffer, 8) == 5 &&
               !buffer[0] && !buffer[1] && !memcmp(buffer + 2, "big", 3));
        VERIFY(!ftruncate(fd, 1048576) && ftruncate(fd, 1048577) < 0 &&
               lseek(fd, -4, SEEK_END) == 1048572 && read(fd, buffer, 8) == 4 &&
               !buffer[0] && !buffer[3] && !close(fd) && !unlink(large_paths[i]));
    }
    puts("[sfmod-user] tmpfs trees/lifetimes passed");
    return 0;
}

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
    if (tree_test() || special_devices()) goto fail;
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
