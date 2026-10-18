#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

/* Filesystem type declaration (defined in devfs.c) */
extern vfs_filesystem_t devfs_fs;

/* Initialize and register devfs */
void devfs_init(void);

/* Called when a new block device is registered - adds /dev entry */
void devfs_add_blkdev(uint8_t dev_id, const char *name);

/* Called when a block device is unregistered - removes /dev entry */
void devfs_remove_blkdev(uint8_t dev_id);

/* Get the root dentry of devfs (for chardev node creation) */
vfs_dentry_t *devfs_get_root(void);
/* Boot-only: find/create one root character node before user processes run. */
vfs_inode_t *devfs_boot_char_node(const char *name);

#endif /* DEVFS_H */
