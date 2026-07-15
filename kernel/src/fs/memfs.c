#include "memfs.h"
#include "string.h"
#include "memory.h"
#include "vga.h"

/* ========================================================================
 * memfs - 基于内存的文件系统
 *
 * 所有文件数据存储在物理页面中。每个文件最多可使用
 * MEMFS_MAX_PAGES 个页面（每文件 1MB）。目录不存储数据 -
 * 其子项由 VFS 目录项树跟踪。
 * ======================================================================== */

/* 下一个 inode 编号 */
static uint64_t next_ino = 1;

/* ---- 辅助函数：分配 memfs inode 数据 ---- */
static memfs_inode_data_t *memfs_alloc_data(void)
{
    memfs_inode_data_t *data = (memfs_inode_data_t *)alloc_page();
    if (!data) return NULL;
    memset(data, 0, PAGE_SIZE);
    return data;
}

/* ---- 辅助函数：释放 memfs inode 数据及所有页面 ---- */
static void memfs_free_data(memfs_inode_data_t *data)
{
    if (!data) return;
    for (uint32_t i = 0; i < data->num_pages; i++) {
        if (data->pages[i])
            free_page(data->pages[i]);
    }
    free_page(data);
}

/* ---- 辅助函数：确保指定索引处的页面已分配 ---- */
static int memfs_ensure_page(memfs_inode_data_t *data, uint32_t page_idx)
{
    if (page_idx >= MEMFS_MAX_PAGES)
        return -1;

    if (!data->pages[page_idx]) {
        data->pages[page_idx] = alloc_page();
        if (!data->pages[page_idx])
            return -2;
        memset(data->pages[page_idx], 0, PAGE_SIZE);
        if (page_idx >= data->num_pages)
            data->num_pages = page_idx + 1;
    }
    return 0;
}

/* ========================================================================
 * memfs inode 操作
 * ======================================================================== */

static int memfs_read(vfs_file_t *file, void *buf, size_t count)
{
    vfs_inode_t *inode = file->inode;
    memfs_inode_data_t *data = (memfs_inode_data_t *)inode->fs_data;
    if (!data) return -1;

    uint64_t remaining = inode->size - file->offset;
    if (remaining > count)
        remaining = count;
    if (remaining == 0)
        return 0;

    size_t done = 0;
    while (done < remaining) {
        uint32_t page_idx = (uint32_t)((file->offset + done) / PAGE_SIZE);
        uint32_t page_off = (uint32_t)((file->offset + done) % PAGE_SIZE);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > remaining - done)
            chunk = remaining - done;

        if (page_idx < data->num_pages && data->pages[page_idx]) {
            memcpy((uint8_t *)buf + done,
                   (uint8_t *)data->pages[page_idx] + page_off, chunk);
        } else {
            /* 页面未分配 - 读取为零 */
            memset((uint8_t *)buf + done, 0, chunk);
        }
        done += chunk;
    }

    file->offset += done;
    return (int)done;
}

static int memfs_write(vfs_file_t *file, const void *buf, size_t count)
{
    vfs_inode_t *inode = file->inode;
    memfs_inode_data_t *data = (memfs_inode_data_t *)inode->fs_data;
    if (!data) return -1;

    uint64_t end_offset = file->offset + count;
    if (end_offset > (uint64_t)MEMFS_MAX_PAGES * PAGE_SIZE) {
        count = (size_t)((uint64_t)MEMFS_MAX_PAGES * PAGE_SIZE - file->offset);
        end_offset = file->offset + count;
    }
    if (count == 0) return 0;

    size_t done = 0;
    while (done < count) {
        uint32_t page_idx = (uint32_t)((file->offset + done) / PAGE_SIZE);
        uint32_t page_off = (uint32_t)((file->offset + done) % PAGE_SIZE);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > count - done)
            chunk = count - done;

        if (memfs_ensure_page(data, page_idx) != 0)
            break;

        memcpy((uint8_t *)data->pages[page_idx] + page_off,
               (const uint8_t *)buf + done, chunk);
        done += chunk;
    }

    file->offset += done;
    if (file->offset > inode->size)
        inode->size = file->offset;

    return (int)done;
}

static int memfs_truncate(vfs_inode_t *inode, uint64_t size)
{
    memfs_inode_data_t *data = (memfs_inode_data_t *)inode->fs_data;
    if (!data) return -1;

    /* 释放超出新大小的页面 */
    uint32_t last_page = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    for (uint32_t i = last_page; i < data->num_pages; i++) {
        if (data->pages[i]) {
            free_page(data->pages[i]);
            data->pages[i] = NULL;
        }
    }
    if (data->num_pages > last_page)
        data->num_pages = last_page;

    inode->size = size;
    return 0;
}

/* ========================================================================
 * memfs 文件系统操作
 * ======================================================================== */

static int memfs_mount(vfs_superblock_t *sb, uint8_t blkdev_id, const char *options)
{
    (void)blkdev_id;
    (void)options;

    /* 创建根 inode */
    vfs_inode_t *root = (vfs_inode_t *)alloc_page();
    if (!root) return -1;
    memset(root, 0, PAGE_SIZE);
    root->ino = next_ino++;
    root->type = VFS_TYPE_DIR;
    root->mode = VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR |
                 VFS_S_IRGRP | VFS_S_IXGRP |
                 VFS_S_IROTH | VFS_S_IXOTH;
    root->size = 0;
    root->nlinks = 2;
    root->sb = sb;

    /* 根目录不需要数据页面 */
    root->fs_data = NULL;

    /* 创建根目录项 */
    vfs_dentry_t *root_dentry = (vfs_dentry_t *)alloc_page();
    if (!root_dentry) {
        free_page(root);
        return -2;
    }
    memset(root_dentry, 0, PAGE_SIZE);
    strcpy(root_dentry->name, "/");
    root_dentry->inode = root;
    root_dentry->parent = root_dentry;  /* 根目录的父目录是自身 */

    sb->root = root_dentry;
    sb->root_inode = root;

    return 0;
}

static int memfs_unmount(vfs_superblock_t *sb)
{
    (void)sb;
    /* 在完整实现中，应遍历所有 inode 并释放。
     * 目前内存会在关机时自动回收。 */
    return 0;
}

static vfs_inode_t *memfs_alloc_inode(vfs_superblock_t *sb)
{
    vfs_inode_t *inode = (vfs_inode_t *)alloc_page();
    if (!inode) return NULL;
    memset(inode, 0, PAGE_SIZE);

    inode->ino = next_ino++;
    inode->sb = sb;
    inode->nlinks = 1;

    /* 设置操作函数 */
    inode->read = memfs_read;
    inode->write = memfs_write;
    inode->truncate = memfs_truncate;

    /* 为文件分配 memfs 专用数据 */
    inode->fs_data = memfs_alloc_data();

    return inode;
}

static void memfs_destroy_inode(vfs_inode_t *inode)
{
    if (inode->fs_data) {
        memfs_free_data((memfs_inode_data_t *)inode->fs_data);
        inode->fs_data = NULL;
    }
    free_page(inode);
}

/* ========================================================================
 * 文件系统类型定义
 * ======================================================================== */

vfs_filesystem_t memfs_fs = {
    .name = "memfs",
    .mount = memfs_mount,
    .unmount = memfs_unmount,
    .alloc_inode = memfs_alloc_inode,
    .destroy_inode = memfs_destroy_inode,
};

/* ========================================================================
 * 初始化
 * ======================================================================== */

void memfs_init(void)
{
    vfs_register_fs(&memfs_fs);
}
