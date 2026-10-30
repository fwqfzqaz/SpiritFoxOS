#include "chardev.h"
#include "devfs.h"
#include "memory.h"
#include "string.h"
#include "vga.h"

/* ========================================================================
 * 字符设备注册表 - Character Device Registry
 *
 * 管理 256 个字符设备槽位，为 DRM、evdev 等提供统一调度。
 * ======================================================================== */

#define CHARDEV_MAX  256

static const chardev_ops_t *chardev_table[CHARDEV_MAX];

/* inode 编号计数器（避免与 devfs 内部计数器冲突） */
static uint64_t chardev_next_ino = 0x10000;

/* ========================================================================
 * 注册表操作
 * ======================================================================== */

void chardev_init(void)
{
    memset(chardev_table, 0, sizeof(chardev_table));
}

int chardev_register(uint32_t id, const chardev_ops_t *ops)
{
    if (id >= CHARDEV_MAX) return -1;
    chardev_table[id] = ops;
    return 0;
}

int chardev_unregister(uint32_t id)
{
    if (id >= CHARDEV_MAX) return -1;
    chardev_table[id] = NULL;
    return 0;
}

const chardev_ops_t *chardev_get_ops(uint32_t id)
{
    if (id >= CHARDEV_MAX) return NULL;
    return chardev_table[id];
}

/* ========================================================================
 * VFS 调度：将文件操作委派给字符设备驱动
 * ======================================================================== */

int chardev_open(vfs_file_t *file)
{
    if (!file || !file->inode) return -1;
    const chardev_ops_t *ops = chardev_get_ops(file->inode->chardev_id);
    if (!ops) return -1;
    if (!ops->open) return 0;
    return ops->open(file);
}

int chardev_close(vfs_file_t *file)
{
    if (!file || !file->inode) return -1;
    const chardev_ops_t *ops = chardev_get_ops(file->inode->chardev_id);
    if (!ops) return -1;
    if (!ops->close) return 0;
    return ops->close(file);
}

int chardev_read(vfs_file_t *file, void *buf, size_t count)
{
    if (!file || !file->inode) return -1;
    const chardev_ops_t *ops = chardev_get_ops(file->inode->chardev_id);
    if (!ops || !ops->read) return -1;
    return ops->read(file, buf, count);
}

int chardev_write(vfs_file_t *file, const void *buf, size_t count)
{
    if (!file || !file->inode) return -1;
    const chardev_ops_t *ops = chardev_get_ops(file->inode->chardev_id);
    if (!ops || !ops->write) return -1;
    return ops->write(file, buf, count);
}

int64_t chardev_ioctl(vfs_file_t *file, uint64_t cmd, uint64_t arg)
{
    if (!file || !file->inode) return -1;
    const chardev_ops_t *ops = chardev_get_ops(file->inode->chardev_id);
    if (!ops || !ops->ioctl) return -1;
    return ops->ioctl(file, cmd, arg);
}

int chardev_mmap(vfs_file_t *file, uint64_t addr, size_t len,
                 int prot, int flags, uint64_t offset)
{
    if (!file || !file->inode) return -1;
    const chardev_ops_t *ops = chardev_get_ops(file->inode->chardev_id);
    if (!ops || !ops->mmap) return -1;
    return ops->mmap(file, addr, len, prot, flags, offset);
}

int chardev_poll(vfs_file_t *file, int events)
{
    if (!file || !file->inode) return 0;
    const chardev_ops_t *ops = chardev_get_ops(file->inode->chardev_id);
    if (!ops || !ops->poll) return 0;
    return ops->poll(file, events);
}

/* ========================================================================
 * 在 devfs 中创建字符设备节点
 *
 * 按路径层级创建目录和字符设备 inode/dentry。
 * 例如 "/dev/dri/card0" 会在 devfs 根下创建 dri/ 目录和 card0 设备节点。
 * ======================================================================== */

int chardev_create_node(const char *path, uint32_t chardev_id, uint32_t mode)
{
    if (devfs_is_sealed()) return -30;
    vfs_dentry_t *root = devfs_get_root();
    if (!root) return -1;

    /* 跳过 "/dev/" 前缀 */
    const char *p = path;
    if (strncmp(p, "/dev/", 5) == 0)
        p += 5;
    else if (p[0] == '/')
        p++;

    vfs_dentry_t *current = root;
    char comp[VFS_MAX_NAME];

    while (*p) {
        /* 提取下一个路径分量 */
        int len = 0;
        while (*p && *p != '/' && len < VFS_MAX_NAME - 1)
            comp[len++] = *p++;
        comp[len] = '\0';
        if (len == 0) {
            if (*p == '/') p++;
            continue;
        }

        if (*p == '/') {
            /* 中间分量 → 查找或创建子目录 */
            p++;

            vfs_dentry_t *child = current->child;
            vfs_dentry_t *found = NULL;
            while (child) {
                if (strcmp(child->name, comp) == 0) {
                    found = child;
                    break;
                }
                child = child->next;
            }

            if (found) {
                current = found;
            } else {
                /* 创建目录 dentry + inode */
                vfs_dentry_t *dir_de = (vfs_dentry_t *)alloc_page();
                if (!dir_de) return -1;
                memset(dir_de, 0, PAGE_SIZE);
                strncpy(dir_de->name, comp, VFS_MAX_NAME - 1);

                vfs_inode_t *dir_ino = (vfs_inode_t *)alloc_page();
                if (!dir_ino) { free_page(dir_de); return -1; }
                memset(dir_ino, 0, PAGE_SIZE);
                dir_ino->ino  = chardev_next_ino++;
                dir_ino->type = VFS_TYPE_DIR;
                dir_ino->mode = VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR |
                                VFS_S_IRGRP | VFS_S_IXGRP |
                                VFS_S_IROTH | VFS_S_IXOTH;
                dir_ino->sb   = root->inode ? root->inode->sb : NULL;

                dir_de->inode  = dir_ino;
                dir_de->parent = current;
                dir_de->next   = current->child;
                current->child = dir_de;
                current = dir_de;
            }
        } else {
            /* 末尾分量 → 创建字符设备节点 */
            vfs_inode_t *cdev_ino = (vfs_inode_t *)alloc_page();
            if (!cdev_ino) return -1;
            memset(cdev_ino, 0, PAGE_SIZE);
            cdev_ino->ino         = chardev_next_ino++;
            cdev_ino->type        = VFS_TYPE_CHARDEV;
            cdev_ino->mode        = mode;
            cdev_ino->chardev_id  = chardev_id;
            cdev_ino->sb          = root->inode ? root->inode->sb : NULL;

            vfs_dentry_t *cdev_de = (vfs_dentry_t *)alloc_page();
            if (!cdev_de) { free_page(cdev_ino); return -1; }
            memset(cdev_de, 0, PAGE_SIZE);
            strncpy(cdev_de->name, comp, VFS_MAX_NAME - 1);
            cdev_de->inode  = cdev_ino;
            cdev_de->parent = current;
            cdev_de->next   = current->child;
            current->child  = cdev_de;

            printf("[chardev] Created %s (id=%d)\n", path, chardev_id);
            return 0;
        }
    }

    return -1;
}
