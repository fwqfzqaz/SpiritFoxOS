#include "devfs.h"
#include "blkdev.h"
#include "string.h"
#include "memory.h"
#include "vga.h"

/* ========================================================================
 * devfs - Device filesystem
 *
 * Auto-populates /dev with block device entries. Reading/writing device
 * files delegates to the blkdev layer.
 * ======================================================================== */

/* Next inode number */
static uint64_t devfs_next_ino = 1;

/* Reference to devfs root dentry (set during mount) */
static vfs_dentry_t *devfs_root_dentry = NULL;

/* ========================================================================
 * devfs block device read/write
 * ======================================================================== */

static int devfs_blkdev_read(vfs_file_t *file, void *buf, size_t count)
{
    (void)file;
    (void)buf;
    (void)count;
    /* Block device read via VFS requires sector-aligned operations.
     * For now, return not-supported. Full implementation would use
     * blkdev_read with the file offset converted to LBA. */
    return -1;
}

static int devfs_blkdev_write(vfs_file_t *file, const void *buf, size_t count)
{
    (void)file;
    (void)buf;
    (void)count;
    return -1;
}

/* ========================================================================
 * devfs filesystem operations
 * ======================================================================== */

static int devfs_mount(vfs_superblock_t *sb, uint8_t blkdev_id, const char *options)
{
    (void)blkdev_id;
    (void)options;

    /* Create root inode */
    vfs_inode_t *root = (vfs_inode_t *)alloc_page();
    if (!root) return -1;
    memset(root, 0, PAGE_SIZE);
    root->ino = devfs_next_ino++;
    root->type = VFS_TYPE_DIR;
    root->mode = VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR |
                 VFS_S_IRGRP | VFS_S_IXGRP |
                 VFS_S_IROTH | VFS_S_IXOTH;
    root->size = 0;
    root->nlinks = 2;
    root->sb = sb;
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
    root_dentry->parent = root_dentry;

    sb->root = root_dentry;
    sb->root_inode = root;

    devfs_root_dentry = root_dentry;

    /* Auto-populate with existing block devices */
    for (int i = 0; i < BLKDEV_MAX_DEVICES; i++) {
        blkdev_t *dev = blkdev_get((uint8_t)i);
        if (dev && dev->in_use) {
            devfs_add_blkdev(dev->id, dev->name);
        }
    }

    /* Add /dev/null */
    {
        vfs_inode_t *null_ino = (vfs_inode_t *)alloc_page();
        if (null_ino) {
            memset(null_ino, 0, PAGE_SIZE);
            null_ino->ino = devfs_next_ino++;
            null_ino->type = VFS_TYPE_CHARDEV;
            null_ino->mode = VFS_S_IRUSR | VFS_S_IWUSR;
            null_ino->chardev_id = 0;  /* minor 0 = null */
            null_ino->sb = sb;
            null_ino->fs_data = NULL;

            vfs_dentry_t *null_de = (vfs_dentry_t *)alloc_page();
            if (null_de) {
                memset(null_de, 0, PAGE_SIZE);
                strcpy(null_de->name, "null");
                null_de->inode = null_ino;
                null_de->parent = root_dentry;
                null_de->next = root_dentry->child;
                root_dentry->child = null_de;
            }
        }
    }

    /* Add /dev/console */
    {
        vfs_inode_t *con_ino = (vfs_inode_t *)alloc_page();
        if (con_ino) {
            memset(con_ino, 0, PAGE_SIZE);
            con_ino->ino = devfs_next_ino++;
            con_ino->type = VFS_TYPE_CHARDEV;
            con_ino->mode = VFS_S_IRUSR | VFS_S_IWUSR;
            con_ino->chardev_id = 1;  /* minor 1 = console */
            con_ino->sb = sb;
            con_ino->fs_data = NULL;

            vfs_dentry_t *con_de = (vfs_dentry_t *)alloc_page();
            if (con_de) {
                memset(con_de, 0, PAGE_SIZE);
                strcpy(con_de->name, "console");
                con_de->inode = con_ino;
                con_de->parent = root_dentry;
                con_de->next = root_dentry->child;
                root_dentry->child = con_de;
            }
        }
    }

    return 0;
}

static int devfs_unmount(vfs_superblock_t *sb)
{
    (void)sb;
    devfs_root_dentry = NULL;
    return 0;
}

static vfs_inode_t *devfs_alloc_inode(vfs_superblock_t *sb)
{
    vfs_inode_t *inode = (vfs_inode_t *)alloc_page();
    if (!inode) return NULL;
    memset(inode, 0, PAGE_SIZE);
    inode->ino = devfs_next_ino++;
    inode->sb = sb;
    return inode;
}

static void devfs_destroy_inode(vfs_inode_t *inode)
{
    free_page(inode);
}

/* ========================================================================
 * Filesystem type definition
 * ======================================================================== */

vfs_filesystem_t devfs_fs = {
    .name = "devfs",
    .mount = devfs_mount,
    .unmount = devfs_unmount,
    .alloc_inode = devfs_alloc_inode,
    .destroy_inode = devfs_destroy_inode,
};

/* ========================================================================
 * Dynamic device entry management
 * ======================================================================== */

void devfs_add_blkdev(uint8_t dev_id, const char *name)
{
    if (!devfs_root_dentry) return;

    /* Find the superblock from root dentry */
    vfs_superblock_t *sb = devfs_root_dentry->inode ?
                            (vfs_superblock_t *)devfs_root_dentry->inode->sb : NULL;

    /* Create block device inode */
    vfs_inode_t *blk_ino = (vfs_inode_t *)alloc_page();
    if (!blk_ino) return;
    memset(blk_ino, 0, PAGE_SIZE);
    blk_ino->ino = devfs_next_ino++;
    blk_ino->type = VFS_TYPE_BLKDEV;
    blk_ino->mode = VFS_S_IRUSR | VFS_S_IWUSR;

    blkdev_t *dev = blkdev_get(dev_id);
    if (dev) {
        blk_ino->size = dev->sector_count * dev->sector_size;
        blk_ino->blkdev_id = dev_id;
    }

    blk_ino->sb = sb;
    blk_ino->fs_data = NULL;
    blk_ino->read = devfs_blkdev_read;
    blk_ino->write = devfs_blkdev_write;

    /* Create dentry */
    vfs_dentry_t *blk_de = (vfs_dentry_t *)alloc_page();
    if (!blk_de) {
        free_page(blk_ino);
        return;
    }
    memset(blk_de, 0, PAGE_SIZE);
    strncpy(blk_de->name, name, VFS_MAX_NAME - 1);
    blk_de->inode = blk_ino;
    blk_de->parent = devfs_root_dentry;

    /* Add to root's child list */
    blk_de->next = devfs_root_dentry->child;
    devfs_root_dentry->child = blk_de;

    printf("[devfs] Added /dev/%s (blkdev %d)\n", name, dev_id);
}

void devfs_remove_blkdev(uint8_t dev_id)
{
    if (!devfs_root_dentry) return;

    /* Find and remove the dentry with matching blkdev_id */
    vfs_dentry_t **pp = &devfs_root_dentry->child;
    while (*pp) {
        vfs_dentry_t *de = *pp;
        if (de->inode && de->inode->type == VFS_TYPE_BLKDEV &&
            de->inode->blkdev_id == dev_id) {
            *pp = de->next;
            printf("[devfs] Removed /dev/%s\n", de->name);
            return;
        }
        pp = &de->next;
    }
}

/* ========================================================================
 * Initialization
 * ======================================================================== */

void devfs_init(void)
{
    vfs_register_fs(&devfs_fs);
}
