#include "syscall_internal.h"
#include "process.h"
#include "vfs.h"
#include "memory.h"
#include "hal.h"
#include "vga.h"
#include "string.h"
#include "terminal.h"
#include "errno.h"
#include "mmu.h"

/* 将 vfs_inode_t 转换为 linux_stat_t */
static void inode_to_stat(const vfs_inode_t *inode, linux_stat_t *stat)
{
    stat->st_dev     = 0;
    stat->st_ino     = inode->ino;
    stat->st_nlink   = inode->nlinks;
    stat->st_mode    = inode->mode;
    stat->st_uid     = inode->uid;
    stat->st_gid     = inode->gid;
    stat->__pad0     = 0;
    stat->st_rdev    = 0;
    stat->st_size    = (int64_t)inode->size;
    stat->st_blksize = 4096;
    stat->st_blocks  = (inode->size + 511) / 512;
    stat->st_atime     = inode->atime;
    stat->st_atime_nsec = 0;
    stat->st_mtime     = inode->mtime;
    stat->st_mtime_nsec = 0;
    stat->st_ctime     = inode->ctime;
    stat->st_ctime_nsec = 0;
}

/* ========================================================================
 * File I/O syscalls
 * ======================================================================== */

int64_t sys_read(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    void *buf = (void *)frame->rsi;
    size_t count = (size_t)frame->rdx;
    (void)buf;
    return vfs_read(fd, buf, count);
}

int64_t sys_write(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    const void *buf = (const void *)frame->rsi;
    size_t count = (size_t)frame->rdx;

    /* 直接写入串口 + VGA 来处理 stdout/stderr。
     * 新的用户进程可能没有在 VFS 中打开 fd 1/2，
     * 因此绕过 VFS 直接写入控制台。 */
    if (fd == 1 || fd == 2) {
        const char *p = (const char *)buf;
        for (size_t i = 0; i < count; i++) {
            /* 写入串口 (COM1) */
            while (!(hal_inb(0x3FD) & 0x20))
                ;
            hal_outb(0x3F8, (uint8_t)p[i]);
            /* 同时写入 VGA 终端 */
            terminal_putchar(p[i]);
        }
        return (int64_t)count;
    }

    return vfs_write(fd, buf, count);
}

int64_t sys_open(trap_frame_t *frame)
{
    const char *path = (const char *)frame->rdi;
    uint32_t flags = (uint32_t)frame->rsi;
    uint32_t mode = (uint32_t)frame->rdx;
    (void)path;
    return vfs_open(path, flags, mode);
}

int64_t sys_close(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    return vfs_close(fd);
}

int64_t sys_fstat(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    linux_stat_t *stat = (linux_stat_t *)frame->rsi;

    vfs_inode_t inode;
    int ret = vfs_fstat(fd, &inode);
    if (ret < 0)
        return ret;

    inode_to_stat(&inode, stat);
    return 0;
}

int64_t sys_lseek(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    int64_t offset = (int64_t)frame->rsi;
    int whence = (int)frame->rdx;
    return vfs_seek(fd, offset, whence);
}

int64_t sys_pipe(trap_frame_t *frame)
{
    int *fd = (int *)frame->rdi;
    (void)fd;
    return vfs_pipe(fd);
}

int64_t sys_dup(trap_frame_t *frame)
{
    int oldfd = (int)frame->rdi;
    return vfs_dup(oldfd);
}

int64_t sys_dup2(trap_frame_t *frame)
{
    int oldfd = (int)frame->rdi;
    int newfd = (int)frame->rsi;
    return vfs_dup2(oldfd, newfd);
}

int64_t sys_getcwd(trap_frame_t *frame)
{
    char *buf = (char *)frame->rdi;
    size_t size = (size_t)frame->rsi;
    process_t *proc = process_current();
    if (!proc)
        return -ESRCH;

    size_t cwd_len = 0;
    while (proc->cwd[cwd_len] && cwd_len < VFS_MAX_PATH)
        cwd_len++;

    if (size < cwd_len + 1)
        return -ERANGE;

    for (size_t i = 0; i <= cwd_len; i++)
        buf[i] = proc->cwd[i];

    return (int64_t)cwd_len;
}

int64_t sys_chdir(trap_frame_t *frame)
{
    const char *path = (const char *)frame->rdi;
    (void)path;
    return vfs_chdir(path);
}

int64_t sys_mkdir(trap_frame_t *frame)
{
    const char *path = (const char *)frame->rdi;
    uint32_t mode = (uint32_t)frame->rsi;
    (void)path;
    return vfs_mkdir(path, mode);
}

int64_t sys_rmdir(trap_frame_t *frame)
{
    const char *path = (const char *)frame->rdi;
    (void)path;
    return vfs_rmdir(path);
}

int64_t sys_unlink(trap_frame_t *frame)
{
    const char *path = (const char *)frame->rdi;
    (void)path;
    return vfs_unlink(path);
}

int64_t sys_stat(trap_frame_t *frame)
{
    const char *path = (const char *)frame->rdi;
    linux_stat_t *stat = (linux_stat_t *)frame->rsi;

    (void)path;

    /* 打开、fstat、关闭 */
    int fd = vfs_open(path, VFS_O_RDONLY, 0);
    if (fd < 0)
        return -ENOENT;

    vfs_inode_t inode;
    int ret = vfs_fstat(fd, &inode);
    vfs_close(fd);

    if (ret < 0)
        return ret;

    inode_to_stat(&inode, stat);
    return 0;
}

int64_t sys_lstat(trap_frame_t *frame)
{
    /* 目前与 stat 相同（VFS 尚不支持符号链接） */
    return sys_stat(frame);
}

int64_t sys_access(trap_frame_t *frame)
{
    const char *path = (const char *)frame->rdi;
    int mode = (int)frame->rsi;

    (void)mode;

    /* Just check if the file existsk if the file exists */
    int fd = vfs_open(path, VFS_O_RDONLY, 0);
    if (fd < 0)
        return -ENOENT;
    vfs_close(fd);
    return 0;
}

int64_t sys_readlink(trap_frame_t *frame)
{
    /* VFS 尚不支持符号链接 */
    return -ENOENT;
}

int64_t sys_newfstatat(trap_frame_t *frame)
{
    int dirfd = (int)frame->rdi;
    const char *pathname = (const char *)frame->rsi;
    linux_stat_t *statbuf = (linux_stat_t *)frame->rdx;
    int flags = (int)frame->r10;

    (void)dirfd;
    (void)flags;

    /* 目前与 stat 相同 */
    int fd = vfs_open(pathname, VFS_O_RDONLY, 0);
    if (fd < 0)
        return -ENOENT;

    vfs_inode_t inode;
    int ret = vfs_fstat(fd, &inode);
    vfs_close(fd);

    if (ret < 0)
        return ret;

    memset(statbuf, 0, sizeof(linux_stat_t));
    inode_to_stat(&inode, statbuf);
    return 0;
}

int64_t sys_readlinkat(trap_frame_t *frame)
{
    int dirfd = (int)frame->rdi;
    const char *pathname = (const char *)frame->rsi;
    char *buf = (char *)frame->rdx;
    size_t bufsiz = (size_t)frame->r10;

    (void)dirfd;
    (void)pathname;
    (void)buf;
    (void)bufsiz;

    /* VFS 尚不支持符号链接 */
    return -ENOENT;
}

int64_t sys_pread64(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    void *buf = (void *)frame->rsi;
    size_t count = (size_t)frame->rdx;
    int64_t offset = (int64_t)frame->r10;
    (void)offset;
    return vfs_read(fd, buf, count);
}

int64_t sys_pwrite64(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    const void *buf = (const void *)frame->rsi;
    size_t count = (size_t)frame->rdx;
    int64_t offset = (int64_t)frame->r10;
    (void)offset;
    return vfs_write(fd, buf, count);
}

int64_t sys_getdents(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    void *dirp = (void *)frame->rsi;
    unsigned int count = (unsigned int)frame->rdx;

    (void)dirp;
    (void)count;

    /* 使用 vfs_readdir 读取目录项 */
    /* 简单桩函数：返回 0（无目录项） */
    (void)fd;
    return 0;
}

int64_t sys_getdents64(trap_frame_t *frame)
{
    return sys_getdents(frame);
}

int64_t sys_rename(trap_frame_t *frame)
{
    (void)frame;
    return -ENOSYS;
}

int64_t sys_chmod(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_fchmod(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_chown(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_fcntl(trap_frame_t *frame)
{
    (void)frame;
    return 0;  /* 桩函数 */
}

int64_t sys_flock(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_fsync(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_fadvise64(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_pipe2(trap_frame_t *frame)
{
    int *pipefd = (int *)frame->rdi;
    int flags = (int)frame->rsi;
    (void)flags;

    /* 使用 VFS 管道 */
    int fds[2];
    int ret = vfs_pipe(fds);
    if (ret < 0)
        return ret;

    if (pipefd) {
        pipefd[0] = fds[0];
        pipefd[1] = fds[1];
    }
    return 0;
}

int64_t sys_dup3(trap_frame_t *frame)
{
    int oldfd = (int)frame->rdi;
    int newfd = (int)frame->rsi;
    int flags = (int)frame->rdx;
    (void)flags;
    return vfs_dup2(oldfd, newfd);
}
