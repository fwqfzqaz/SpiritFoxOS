#ifndef MEMFS_H
#define MEMFS_H

#include "vfs.h"

/* Maximum data pages per file (256 * 4096 = 1MB per file) */
#define MEMFS_MAX_PAGES 256

/* Per-inode memfs data */
typedef struct {
    void    *pages[MEMFS_MAX_PAGES];  /* Data pages */
    uint32_t num_pages;                /* Number of allocated pages */
} memfs_inode_data_t;

/* Filesystem type declaration (defined in memfs.c) */
extern vfs_filesystem_t memfs_fs;

/* Initialize and register memfs */
void memfs_init(void);

#endif /* MEMFS_H */
