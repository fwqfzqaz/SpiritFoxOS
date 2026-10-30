#ifndef CHARDEV_H
#define CHARDEV_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"

/* ========================================================================
 * 字符设备框架 - Character Device Framework
 *
 * 为 DRM、evdev 等字符设备提供统一的操作接口。
 * 字符设备通过 chardev_id 标识，在 devfs 中创建设备节点。
 * ======================================================================== */

/* 字符设备操作表 */
typedef struct chardev_ops {
    int      (*open)(vfs_file_t *file);                                    /* 打开设备 */
    int      (*close)(vfs_file_t *file);                                   /* 关闭设备 */
    int      (*read)(vfs_file_t *file, void *buf, size_t count);           /* 读取数据 */
    int      (*write)(vfs_file_t *file, const void *buf, size_t count);    /* 写入数据 */
    int64_t  (*ioctl)(vfs_file_t *file, uint64_t cmd, uint64_t arg);       /* 设备控制 */
    int      (*mmap)(vfs_file_t *file, uint64_t addr, size_t len,          /* 内存映射 */
                     int prot, int flags, uint64_t offset);
    int      (*poll)(vfs_file_t *file, int events);                        /* 轮询事件 */
} chardev_ops_t;

/* 字符设备 ID 分配范围 */
#define CHARDEV_ID_NULL       0     /* /dev/null */
#define CHARDEV_ID_CONSOLE    1     /* /dev/console */
#define CHARDEV_ID_ZERO       2     /* /dev/zero */
/* 3-63: 预留给系统设备 */
/* 64-127: DRM 设备 */
#define CHARDEV_ID_DRM_BASE   64
#define CHARDEV_ID_DRM_CARD0  64    /* /dev/dri/card0 */
#define CHARDEV_ID_DRM_CTRL   65    /* /dev/dri/controlD64 */
#define CHARDEV_ID_DRM_RENDER 66    /* /dev/dri/renderD128 */
/* 128-191: 输入设备 */
#define CHARDEV_ID_INPUT_BASE 128
#define CHARDEV_ID_INPUT_MAX  160   /* 最多 32 个输入设备 */

/* 初始化字符设备子系统 */
void chardev_init(void);

/* 注册字符设备 */
int chardev_register(uint32_t id, const chardev_ops_t *ops);

/* 注销字符设备 */
int chardev_unregister(uint32_t id);

/* 获取字符设备操作表 */
const chardev_ops_t *chardev_get_ops(uint32_t id);

/* VFS 调用：对字符设备执行 ioctl */
int64_t chardev_ioctl(vfs_file_t *file, uint64_t cmd, uint64_t arg);

/* VFS 调用：对字符设备执行 read */
int chardev_read(vfs_file_t *file, void *buf, size_t count);

/* VFS 调用：对字符设备执行 write */
int chardev_write(vfs_file_t *file, const void *buf, size_t count);

/* VFS 调用：对字符设备执行 mmap */
int chardev_mmap(vfs_file_t *file, uint64_t addr, size_t len,
                 int prot, int flags, uint64_t offset);

/* VFS 调用：对字符设备执行 poll */
int chardev_poll(vfs_file_t *file, int events);

/* VFS 调用：打开字符设备 */
int chardev_open(vfs_file_t *file);

/* VFS 调用：关闭字符设备 */
int chardev_close(vfs_file_t *file);

/* 在 devfs 中创建字符设备节点 */
int chardev_create_node(const char *path, uint32_t chardev_id, uint32_t mode);

#endif /* CHARDEV_H */
