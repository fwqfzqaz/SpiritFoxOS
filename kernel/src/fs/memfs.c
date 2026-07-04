#include "memfs.h"
#include "string.h"
#include "memory.h"
#include "vga.h"

/* ========================================================================
 * memfs - Memory-based filesystem
 *
 * All file data is stored in physical pages. Each file can use up to
 * MEMFS_MAX_PAGES pages (1MB per file). Directories store no data -
 * their children are tracked by the VFS dentry tree.
 * ======================================================================== */

/* Next inode number */
static uint64_t next_ino = 1;

/* ---- Helper: allocate memfs inode data ---- */
static memfs_inode_data_t *memfs_alloc_data(void)
{
    memfs_inode_data_t *data = (memfs_inode_data_t *)alloc_page();
    if (!data) return NULL;
    memset(data, 0, PAGE_SIZE);
    return data;
}

/* ---- Helper: free memfs inode data and all pages ---- */
static void memfs_free_data(memfs_inode_data_t *data)
{
    if (!data) return;
    for (uint32_t i = 0; i < data->num_pages; i++) {
        if (data->pages[i])
            free_page(data->pages[i]);
    }
    free_page(data);
}

/* ---- Helper: ensure a page is allocated at given index ---- */
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
 * memfs inode operations
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
            /* Page not allocated - read as zeros */
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

    /* Free pages beyond new size */
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
 * memfs filesystem operations
 * ======================================================================== */

static int memfs_mount(vfs_superblock_t *sb, uint8_t blkdev_id, const char *options)
{
    (void)blkdev_id;
    (void)options;

    /* Create root inode */
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

    /* Root directories don't need data pages */
    root->fs_data = NULL;

    /* Create root dentry */
    vfs_dentry_t *root_dentry = (vfs_dentry_t *)alloc_page();
    if (!root_dentry) {
        free_page(root);
        return -2;
    }
    memset(root_dentry, 0, PAGE_SIZE);
    strcpy(root_dentry->name, "/");
    root_dentry->inode = root;
    root_dentry->parent = root_dentry;  /* Root's parent is itself */

    sb->root = root_dentry;
    sb->root_inode = root;

    return 0;
}

static int memfs_unmount(vfs_superblock_t *sb)
{
    (void)sb;
    /* In a full implementation, we'd walk all inodes and free them.
     * For now, memory is reclaimed on shutdown anyway. */
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

    /* Set up operations */
    inode->read = memfs_read;
    inode->write = memfs_write;
    inode->truncate = memfs_truncate;

    /* Allocate memfs-specific data for files */
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
 * Filesystem type definition
 * ======================================================================== */

vfs_filesystem_t memfs_fs = {
    .name = "memfs",
    .mount = memfs_mount,
    .unmount = memfs_unmount,
    .alloc_inode = memfs_alloc_inode,
    .destroy_inode = memfs_destroy_inode,
};

/* ========================================================================
 * Initialization
 * ======================================================================== */

void memfs_init(void)
{
    vfs_register_fs(&memfs_fs);
}
