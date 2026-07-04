#include "fat32.h"
#include "blkdev.h"
#include "string.h"
#include "memory.h"
#include "vga.h"

/* ========================================================================
 * fat32 - FAT32 filesystem driver with read/write support
 *
 * Interfaces with the VFS layer to provide directory listing, file
 * reading, file writing, file/directory creation and deletion from a
 * FAT32-formatted block device.
 * ======================================================================== */

/* Next inode number */
static uint64_t fat32_next_ino = 1;

/* ========================================================================
 * Low-level helpers
 * ======================================================================== */

/* Read a full cluster from disk into buf.
 * buf must be at least (sectors_per_cluster * bytes_per_sector) bytes.
 * Returns 0 on success, negative on error. */
static int fat32_read_cluster(fat32_sb_data_t *sb_data, uint32_t cluster, void *buf)
{
    if (cluster < 2)
        return -1;

    uint64_t lba = sb_data->data_start_sector +
                   (uint64_t)(cluster - 2) * sb_data->sectors_per_cluster;
    uint32_t count = sb_data->sectors_per_cluster;

    return blkdev_read(sb_data->blkdev_id, lba, count, buf);
}

/* Write a full cluster from buf to disk.
 * buf must be at least (sectors_per_cluster * bytes_per_sector) bytes.
 * Returns 0 on success, negative on error. */
static int fat32_write_cluster(fat32_sb_data_t *sb_data, uint32_t cluster, const void *buf)
{
    if (cluster < 2)
        return -1;

    uint64_t lba = sb_data->data_start_sector +
                   (uint64_t)(cluster - 2) * sb_data->sectors_per_cluster;
    uint32_t count = sb_data->sectors_per_cluster;

    return blkdev_write(sb_data->blkdev_id, lba, count, buf);
}

/* Read the FAT table entry for a given cluster.
 * Returns the next cluster number, or a value >= FAT32_EOC_MIN for end-of-chain. */
static uint32_t fat32_get_fat_entry(fat32_sb_data_t *sb_data, uint32_t cluster)
{
    /* Each FAT entry is 4 bytes */
    uint64_t fat_offset = (uint64_t)cluster * 4;
    uint64_t fat_sector = sb_data->fat_start_sector + fat_offset / sb_data->bytes_per_sector;
    uint32_t entry_offset = fat_offset % sb_data->bytes_per_sector;

    /* Read the sector containing the FAT entry */
    /* We use a full page as a scratch buffer */
    void *sector_buf = alloc_page();
    if (!sector_buf)
        return 0x0FFFFFFF; /* Return EOC on allocation failure */

    int ret = blkdev_read(sb_data->blkdev_id, fat_sector, 1, sector_buf);
    if (ret != 0) {
        free_page(sector_buf);
        return 0x0FFFFFFF;
    }

    uint32_t entry = *(uint32_t *)((uint8_t *)sector_buf + entry_offset);
    free_page(sector_buf);

    /* FAT32 uses only lower 28 bits */
    return entry & 0x0FFFFFFF;
}

/* Write a FAT table entry for a given cluster.
 * Preserves the top 4 bits of the existing entry.
 * Returns 0 on success, negative on error. */
static int fat32_set_fat_entry(fat32_sb_data_t *sb_data, uint32_t cluster, uint32_t value)
{
    /* Each FAT entry is 4 bytes */
    uint64_t fat_offset = (uint64_t)cluster * 4;
    uint64_t fat_sector = sb_data->fat_start_sector + fat_offset / sb_data->bytes_per_sector;
    uint32_t entry_offset = fat_offset % sb_data->bytes_per_sector;

    /* Read the sector containing the FAT entry */
    void *sector_buf = alloc_page();
    if (!sector_buf)
        return -1;

    int ret = blkdev_read(sb_data->blkdev_id, fat_sector, 1, sector_buf);
    if (ret != 0) {
        free_page(sector_buf);
        return -2;
    }

    /* Modify the entry, preserving top 4 bits */
    uint32_t *entry_ptr = (uint32_t *)((uint8_t *)sector_buf + entry_offset);
    *entry_ptr = (*entry_ptr & 0xF0000000) | (value & 0x0FFFFFFF);

    /* Write the sector back */
    ret = blkdev_write(sb_data->blkdev_id, fat_sector, 1, sector_buf);
    free_page(sector_buf);

    return ret;
}

/* Allocate a free cluster from the FAT table.
 * Scans from next_free_cluster hint.
 * Marks the allocated cluster as end-of-chain.
 * Returns 0 on success, negative on error. */
static int fat32_alloc_cluster(fat32_sb_data_t *sb_data, uint32_t *out_cluster)
{
    uint32_t start = sb_data->next_free_cluster;
    if (start < 2) start = 2;

    /* Scan FAT entries for a free one (value == 0) */
    for (uint32_t i = start; i < sb_data->total_clusters + 2; i++) {
        uint32_t val = fat32_get_fat_entry(sb_data, i);
        if (val == 0) {
            /* Found a free cluster - mark it as end-of-chain */
            int ret = fat32_set_fat_entry(sb_data, i, 0x0FFFFFFF);
            if (ret != 0)
                return ret;
            *out_cluster = i;
            sb_data->next_free_cluster = i + 1;
            if (sb_data->next_free_cluster >= sb_data->total_clusters + 2)
                sb_data->next_free_cluster = 2;
            return 0;
        }
    }

    /* Wrap around and try from cluster 2 */
    for (uint32_t i = 2; i < start; i++) {
        uint32_t val = fat32_get_fat_entry(sb_data, i);
        if (val == 0) {
            int ret = fat32_set_fat_entry(sb_data, i, 0x0FFFFFFF);
            if (ret != 0)
                return ret;
            *out_cluster = i;
            sb_data->next_free_cluster = i + 1;
            if (sb_data->next_free_cluster >= sb_data->total_clusters + 2)
                sb_data->next_free_cluster = 2;
            return 0;
        }
    }

    return -1;  /* No free clusters */
}

/* Append a new cluster to the end of a cluster chain.
 * last_cluster is the current last cluster in the chain.
 * Returns 0 on success, negative on error. */
static int fat32_append_cluster(fat32_sb_data_t *sb_data, uint32_t last_cluster, uint32_t *out_new_cluster)
{
    uint32_t new_cluster;
    int ret = fat32_alloc_cluster(sb_data, &new_cluster);
    if (ret != 0)
        return ret;

    /* Update the last cluster's FAT entry to point to the new one */
    ret = fat32_set_fat_entry(sb_data, last_cluster, new_cluster);
    if (ret != 0) {
        /* Free the allocated cluster on failure */
        fat32_set_fat_entry(sb_data, new_cluster, 0);
        return ret;
    }

    /* Zero out the new cluster so we don't leak old data */
    void *zero_buf = alloc_page();
    if (zero_buf) {
        memset(zero_buf, 0, PAGE_SIZE);
        fat32_write_cluster(sb_data, new_cluster, zero_buf);
        free_page(zero_buf);
    }

    *out_new_cluster = new_cluster;
    return 0;
}

/* Convert an 8.3 filename to a null-terminated string.
 * Removes trailing spaces, inserts '.' between name and extension. */
static void fat32_format_83_name(const uint8_t *name83, char *out, int out_len)
{
    int i = 0;
    int pos = 0;

    /* Copy name part (8 bytes), strip trailing spaces */
    for (i = 7; i >= 0 && name83[i] == ' '; i--) {}
    int name_end = i;

    for (i = 0; i <= name_end && pos < out_len - 1; i++) {
        char c = (char)name83[i];
        /* Convert to lowercase for display (FAT32 stores uppercase) */
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        out[pos++] = c;
    }

    /* Extension part (3 bytes), strip trailing spaces */
    for (i = 2; i >= 0 && name83[8 + i] == ' '; i--) {}
    int ext_end = i;

    if (ext_end >= 0) {
        if (pos < out_len - 1)
            out[pos++] = '.';
        for (i = 0; i <= ext_end && pos < out_len - 1; i++) {
            char c = (char)name83[8 + i];
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            out[pos++] = c;
        }
    }

    out[pos] = '\0';
}

/* Compare an 8.3 FAT name with a null-terminated search name.
 * The search name is case-insensitive. */
static int fat32_name_match(const uint8_t *name83, const char *search)
{
    char formatted[VFS_MAX_NAME];
    fat32_format_83_name(name83, formatted, VFS_MAX_NAME);
    return strcmp(formatted, search) == 0;
}

/* Convert a null-terminated filename to 8.3 FAT format.
 * Converts to uppercase, pads with spaces.
 * out83 must be at least 11 bytes. */
static void fat32_name_to_83(const char *name, uint8_t *out83)
{
    /* Initialize with spaces */
    memset(out83, ' ', 11);

    int i = 0;
    int pos = 0;

    /* Copy name part (up to 8 chars, stop at '.') */
    while (name[i] && name[i] != '.' && pos < 8) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        out83[pos++] = (uint8_t)c;
        i++;
    }

    /* Skip to extension */
    if (name[i] == '.') {
        i++;
        pos = 8;
        while (name[i] && pos < 11) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            out83[pos++] = (uint8_t)c;
            i++;
        }
    }
}

/* ========================================================================
 * Directory iteration and path resolution
 * ======================================================================== */

/* Iterate directory entries in a cluster chain.
 * On success, fills in entry with the found directory entry and
 * sets *out_cluster to the cluster containing the entry.
 * Returns 0 on found, negative on not found. */
static int fat32_dir_lookup(fat32_sb_data_t *sb_data, uint32_t dir_cluster,
                            const char *name, fat32_dir_entry_t *entry)
{
    uint32_t cluster = dir_cluster;
    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -1;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            free_page(cluster_buf);
            return -2;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t first_byte = entries[i].name[0];

            /* End of directory */
            if (first_byte == 0x00) {
                free_page(cluster_buf);
                return -1;
            }

            /* Deleted entry - skip */
            if (first_byte == 0xE5)
                continue;

            /* Skip LFN entries */
            if (entries[i].attr == FAT32_ATTR_LFN)
                continue;

            /* Skip volume ID */
            if (entries[i].attr & FAT32_ATTR_VOLUME_ID)
                continue;

            /* Check if name matches */
            if (fat32_name_match(entries[i].name, name)) {
                if (entry)
                    memcpy(entry, &entries[i], sizeof(fat32_dir_entry_t));
                free_page(cluster_buf);
                return 0;
            }
        }

        /* Follow cluster chain */
        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    free_page(cluster_buf);
    return -1;  /* Not found */
}

/* Resolve a path from the root directory.
 * Fills in start_cluster and file_size with the result.
 * Returns 0 on success, negative on failure. */
static int __attribute__((unused)) fat32_resolve(fat32_sb_data_t *sb_data, const char *path,
                         uint32_t *start_cluster, uint32_t *file_size, uint32_t *is_dir)
{
    if (!path || path[0] == '\0')
        return -1;

    /* Skip leading slashes */
    const char *p = path;
    while (*p == '/') p++;

    /* If empty path after slashes, return root dir */
    if (*p == '\0') {
        *start_cluster = sb_data->root_dir_cluster;
        *file_size = 0;
        *is_dir = 1;
        return 0;
    }

    /* Walk path components */
    uint32_t current_cluster = sb_data->root_dir_cluster;

    while (*p) {
        /* Extract next path component */
        char component[VFS_MAX_NAME];
        int ci = 0;
        while (*p && *p != '/' && ci < VFS_MAX_NAME - 1) {
            component[ci++] = *p++;
        }
        component[ci] = '\0';

        /* Skip slashes */
        while (*p == '/') p++;

        /* Look up this component in the current directory */
        fat32_dir_entry_t entry;
        int ret = fat32_dir_lookup(sb_data, current_cluster, component, &entry);
        if (ret != 0)
            return -2;  /* Not found */

        /* Get cluster from entry */
        uint32_t entry_cluster = (uint32_t)entry.cluster_hi << 16 | entry.cluster_lo;

        /* Is this the last component? */
        if (*p == '\0') {
            *start_cluster = entry_cluster;
            *file_size = entry.file_size;
            *is_dir = (entry.attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;
            return 0;
        }

        /* Intermediate component must be a directory */
        if (!(entry.attr & FAT32_ATTR_DIRECTORY))
            return -3;  /* Not a directory */

        current_cluster = entry_cluster;
    }

    return -1;
}

/* ========================================================================
 * FAT32 inode read operation
 * ======================================================================== */

static int fat32_inode_read(vfs_file_t *file, void *buf, size_t count)
{
    vfs_inode_t *inode = file->inode;
    if (!inode || !inode->fs_data || !inode->sb || !inode->sb->fs_data)
        return -1;

    fat32_inode_data_t *inode_data = (fat32_inode_data_t *)inode->fs_data;
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)inode->sb->fs_data;

    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;
    uint64_t file_size = inode_data->file_size;

    /* Calculate remaining bytes */
    uint64_t remaining = file_size - file->offset;
    if (remaining > count)
        remaining = count;
    if (remaining == 0)
        return 0;

    /* Walk cluster chain to find the cluster containing the current offset */
    uint32_t cluster = inode_data->start_cluster;
    uint64_t offset = file->offset;

    /* Skip whole clusters to reach the right one */
    while (offset >= cluster_size && cluster >= 2 && cluster < FAT32_EOC_MIN) {
        offset -= cluster_size;
        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    if (cluster < 2 || cluster >= FAT32_EOC_MIN)
        return 0;

    /* Read data cluster by cluster */
    size_t done = 0;
    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -2;

    while (done < remaining && cluster >= 2 && cluster < FAT32_EOC_MIN) {
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            break;
        }

        /* How much to copy from this cluster */
        uint32_t in_cluster = cluster_size - (uint32_t)offset;
        size_t to_copy = remaining - done;
        if (to_copy > in_cluster)
            to_copy = in_cluster;

        memcpy((uint8_t *)buf + done,
               (uint8_t *)cluster_buf + offset, to_copy);

        done += to_copy;
        offset = 0;  /* Subsequent clusters start at offset 0 */

        /* Follow chain */
        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    free_page(cluster_buf);
    file->offset += done;
    return (int)done;
}

/* ========================================================================
 * Directory entry write helpers
 * ======================================================================== */

/* Update an existing directory entry on disk (e.g., file size change).
 * Searches parent_cluster's directory entries for one matching name83,
 * then updates the file_size field and writes back.
 * Returns 0 on success, negative on error. */
static int fat32_update_direntry(fat32_sb_data_t *sb_data, uint32_t parent_cluster,
                                  const uint8_t *name83, uint32_t new_start_cluster,
                                  uint32_t new_file_size)
{
    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -1;

    uint32_t cluster = parent_cluster;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            free_page(cluster_buf);
            return -2;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t first_byte = entries[i].name[0];
            if (first_byte == 0x00) {
                free_page(cluster_buf);
                return -1;  /* Not found */
            }
            if (first_byte == 0xE5)
                continue;
            if (entries[i].attr == FAT32_ATTR_LFN)
                continue;
            if (entries[i].attr & FAT32_ATTR_VOLUME_ID)
                continue;

            /* Compare 8.3 name */
            if (memcmp(entries[i].name, name83, 11) == 0) {
                /* Found - update the entry */
                entries[i].file_size = new_file_size;
                entries[i].cluster_hi = (uint16_t)(new_start_cluster >> 16);
                entries[i].cluster_lo = (uint16_t)(new_start_cluster & 0xFFFF);

                /* Write the cluster back */
                ret = fat32_write_cluster(sb_data, cluster, cluster_buf);
                free_page(cluster_buf);
                return ret;
            }
        }

        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    free_page(cluster_buf);
    return -1;  /* Not found */
}

/* Create a new directory entry in a parent directory's cluster chain.
 * Finds a free slot (deleted entry or end-of-entries), or extends the chain.
 * Returns 0 on success, negative on error. */
static int fat32_create_direntry(fat32_sb_data_t *sb_data, uint32_t parent_cluster,
                                  const char *name, uint32_t start_cluster,
                                  uint32_t file_size, uint8_t attr)
{
    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    /* Build the 8.3 name */
    uint8_t name83[11];
    fat32_name_to_83(name, name83);

    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -1;

    uint32_t cluster = parent_cluster;
    uint32_t prev_cluster = 0;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            free_page(cluster_buf);
            return -2;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            /* Find a free slot: deleted entry (0xE5) or end marker (0x00) */
            if (entries[i].name[0] == 0xE5 || entries[i].name[0] == 0x00) {
                int was_end = (entries[i].name[0] == 0x00);

                /* Fill in the directory entry */
                memcpy(entries[i].name, name83, 11);
                entries[i].attr = attr;
                entries[i].nt_reserved = 0;
                entries[i].create_time_tenth = 0;
                entries[i].create_time = 0;
                entries[i].create_date = 0;
                entries[i].access_date = 0;
                entries[i].cluster_hi = (uint16_t)(start_cluster >> 16);
                entries[i].modify_time = 0;
                entries[i].modify_date = 0;
                entries[i].cluster_lo = (uint16_t)(start_cluster & 0xFFFF);
                entries[i].file_size = file_size;

                /* If we're using the end-of-entries slot, make sure the
                 * next entry is also marked as end-of-entries if there is room */
                if (was_end && i + 1 < entries_per_cluster) {
                    entries[i + 1].name[0] = 0x00;
                }

                /* Write the cluster back */
                ret = fat32_write_cluster(sb_data, cluster, cluster_buf);
                free_page(cluster_buf);
                return ret;
            }
        }

        prev_cluster = cluster;
        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    /* No free slot found - extend the directory's cluster chain */
    if (prev_cluster < 2 || prev_cluster >= FAT32_EOC_MIN) {
        free_page(cluster_buf);
        return -3;  /* Can't extend */
    }

    uint32_t new_cluster;
    int ret = fat32_append_cluster(sb_data, prev_cluster, &new_cluster);
    if (ret != 0) {
        free_page(cluster_buf);
        return ret;
    }

    /* Zero the new cluster (already done by fat32_append_cluster) and write the entry */
    memset(cluster_buf, 0, cluster_size);
    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
    memcpy(entries[0].name, name83, 11);
    entries[0].attr = attr;
    entries[0].nt_reserved = 0;
    entries[0].create_time_tenth = 0;
    entries[0].create_time = 0;
    entries[0].create_date = 0;
    entries[0].access_date = 0;
    entries[0].cluster_hi = (uint16_t)(start_cluster >> 16);
    entries[0].modify_time = 0;
    entries[0].modify_date = 0;
    entries[0].cluster_lo = (uint16_t)(start_cluster & 0xFFFF);
    entries[0].file_size = file_size;
    /* Mark next entry as end-of-entries */
    entries[1].name[0] = 0x00;

    ret = fat32_write_cluster(sb_data, new_cluster, cluster_buf);
    free_page(cluster_buf);
    return ret;
}

/* ========================================================================
 * FAT32 inode write operation
 * ======================================================================== */

static int fat32_inode_write(vfs_file_t *file, const void *buf, size_t count)
{
    vfs_inode_t *inode = file->inode;
    if (!inode || !inode->fs_data || !inode->sb || !inode->sb->fs_data)
        return -1;

    fat32_inode_data_t *inode_data = (fat32_inode_data_t *)inode->fs_data;
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)inode->sb->fs_data;

    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;

    /* If file has no clusters yet, allocate the first one */
    if (inode_data->start_cluster == 0 || inode_data->start_cluster < 2) {
        uint32_t first_cluster;
        int ret = fat32_alloc_cluster(sb_data, &first_cluster);
        if (ret != 0)
            return -2;
        inode_data->start_cluster = first_cluster;
    }

    /* Walk cluster chain to find the cluster containing the current offset.
     * If offset is beyond the current chain, extend it. */
    uint64_t offset = file->offset;
    uint32_t cluster = inode_data->start_cluster;
    uint64_t offset_remaining = offset;

    while (offset_remaining >= cluster_size) {
        uint32_t next = fat32_get_fat_entry(sb_data, cluster);
        if (next >= FAT32_EOC_MIN) {
            /* Need to extend the chain */
            uint32_t new_cluster;
            int ret = fat32_append_cluster(sb_data, cluster, &new_cluster);
            if (ret != 0)
                return -3;
            cluster = new_cluster;
        } else {
            cluster = next;
        }
        offset_remaining -= cluster_size;
    }

    /* Now cluster is the cluster containing our write offset,
     * and offset_remaining is the offset within that cluster. */

    /* Read-modify-write: read the cluster, overlay new data, write back */
    size_t done = 0;
    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -4;

    while (done < count) {
        /* Read the current cluster */
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            break;
        }

        /* How much to write in this cluster */
        uint32_t in_cluster = cluster_size - (uint32_t)offset_remaining;
        size_t to_copy = count - done;
        if (to_copy > in_cluster)
            to_copy = in_cluster;

        /* Overlay the new data */
        memcpy((uint8_t *)cluster_buf + offset_remaining,
               (const uint8_t *)buf + done, to_copy);

        /* Write the cluster back */
        ret = fat32_write_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            break;
        }

        done += to_copy;
        offset_remaining = 0;  /* Subsequent clusters start at offset 0 */

        /* If we need to write more, follow/extend the chain */
        if (done < count) {
            uint32_t next = fat32_get_fat_entry(sb_data, cluster);
            if (next >= FAT32_EOC_MIN) {
                uint32_t new_cluster;
                ret = fat32_append_cluster(sb_data, cluster, &new_cluster);
                if (ret != 0)
                    break;
                cluster = new_cluster;
            } else {
                cluster = next;
            }
        }
    }

    free_page(cluster_buf);

    /* Update file offset and size */
    file->offset += done;
    uint64_t new_size = file->offset;
    if (new_size > inode_data->file_size)
        inode_data->file_size = (uint32_t)new_size;
    if (new_size > inode->size)
        inode->size = new_size;

    /* Update the directory entry on disk */
    fat32_update_direntry(sb_data, inode_data->parent_cluster,
                          inode_data->name83, inode_data->start_cluster,
                          inode_data->file_size);

    return (int)done;
}

/* ========================================================================
 * FAT32 VFS create/mkdir/unlink callbacks
 * ======================================================================== */

static int fat32_create(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{
    if (!parent || !parent->inode || !parent->inode->fs_data || !parent->inode->sb)
        return -1;

    fat32_inode_data_t *parent_data = (fat32_inode_data_t *)parent->inode->fs_data;
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)parent->inode->sb->fs_data;
    fat32_inode_data_t *inode_data = (fat32_inode_data_t *)inode->fs_data;

    /* Create directory entry on disk with start_cluster=0 (no data yet) */
    int ret = fat32_create_direntry(sb_data, parent_data->start_cluster,
                                     name, 0, 0, FAT32_ATTR_ARCHIVE);
    if (ret != 0)
        return ret;

    /* Store the 8.3 name and parent cluster for future updates */
    fat32_name_to_83(name, inode_data->name83);
    inode_data->parent_cluster = parent_data->start_cluster;
    inode_data->start_cluster = 0;
    inode_data->file_size = 0;

    /* Set up inode operations */
    inode->read = fat32_inode_read;
    inode->write = fat32_inode_write;

    return 0;
}

static int fat32_mkdir_cb(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{
    if (!parent || !parent->inode || !parent->inode->fs_data || !parent->inode->sb)
        return -1;

    fat32_inode_data_t *parent_data = (fat32_inode_data_t *)parent->inode->fs_data;
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)parent->inode->sb->fs_data;
    fat32_inode_data_t *inode_data = (fat32_inode_data_t *)inode->fs_data;

    /* Allocate a cluster for the new directory */
    uint32_t dir_cluster;
    int ret = fat32_alloc_cluster(sb_data, &dir_cluster);
    if (ret != 0)
        return ret;

    /* Zero out the new cluster and write "." and ".." entries */
    void *cluster_buf = alloc_page();
    if (!cluster_buf) {
        fat32_set_fat_entry(sb_data, dir_cluster, 0);
        return -2;
    }
    memset(cluster_buf, 0, PAGE_SIZE);

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;

    /* "." entry */
    memset(entries[0].name, ' ', 11);
    entries[0].name[0] = '.';
    entries[0].attr = FAT32_ATTR_DIRECTORY;
    entries[0].cluster_hi = (uint16_t)(dir_cluster >> 16);
    entries[0].cluster_lo = (uint16_t)(dir_cluster & 0xFFFF);
    entries[0].file_size = 0;

    /* ".." entry */
    memset(entries[1].name, ' ', 11);
    entries[1].name[0] = '.';
    entries[1].name[1] = '.';
    entries[1].attr = FAT32_ATTR_DIRECTORY;
    entries[1].cluster_hi = (uint16_t)(parent_data->start_cluster >> 16);
    entries[1].cluster_lo = (uint16_t)(parent_data->start_cluster & 0xFFFF);
    entries[1].file_size = 0;

    /* End-of-entries marker */
    entries[2].name[0] = 0x00;

    /* Write the cluster to disk */
    ret = fat32_write_cluster(sb_data, dir_cluster, cluster_buf);
    free_page(cluster_buf);
    if (ret != 0) {
        fat32_set_fat_entry(sb_data, dir_cluster, 0);
        return ret;
    }

    /* Create directory entry in parent */
    ret = fat32_create_direntry(sb_data, parent_data->start_cluster,
                                 name, dir_cluster, 0, FAT32_ATTR_DIRECTORY);
    if (ret != 0) {
        fat32_set_fat_entry(sb_data, dir_cluster, 0);
        return ret;
    }

    /* Update inode data */
    fat32_name_to_83(name, inode_data->name83);
    inode_data->parent_cluster = parent_data->start_cluster;
    inode_data->start_cluster = dir_cluster;
    inode_data->file_size = 0;

    return 0;
}

static int fat32_unlink_cb(vfs_dentry_t *dentry)
{
    if (!dentry || !dentry->inode || !dentry->inode->fs_data || !dentry->inode->sb)
        return -1;

    fat32_inode_data_t *inode_data = (fat32_inode_data_t *)dentry->inode->fs_data;
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)dentry->inode->sb->fs_data;

    /* Mark the directory entry as deleted (first byte = 0xE5) */
    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -2;

    uint32_t cluster = inode_data->parent_cluster;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            free_page(cluster_buf);
            return -3;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) {
                free_page(cluster_buf);
                return -4;  /* Entry not found */
            }
            if (entries[i].name[0] == 0xE5)
                continue;
            if (entries[i].attr == FAT32_ATTR_LFN)
                continue;
            if (entries[i].attr & FAT32_ATTR_VOLUME_ID)
                continue;

            if (memcmp(entries[i].name, inode_data->name83, 11) == 0) {
                /* Found - mark as deleted */
                entries[i].name[0] = 0xE5;
                ret = fat32_write_cluster(sb_data, cluster, cluster_buf);
                free_page(cluster_buf);

                /* Free all clusters in the file's chain */
                if (inode_data->start_cluster >= 2) {
                    uint32_t cl = inode_data->start_cluster;
                    while (cl >= 2 && cl < FAT32_EOC_MIN) {
                        uint32_t next = fat32_get_fat_entry(sb_data, cl);
                        fat32_set_fat_entry(sb_data, cl, 0);
                        cl = next;
                    }
                }

                return ret;
            }
        }

        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    free_page(cluster_buf);
    return -4;  /* Entry not found */
}

/* ========================================================================
 * Directory population helper
 *
 * Reads all directory entries from a FAT32 directory cluster chain and
 * creates VFS dentry/inode pairs under the given parent dentry.
 * ======================================================================== */

/* ========================================================================
 * LFN (Long File Name) support
 *
 * LFN entries precede the 8.3 entry and store the long name in UCS-2.
 * Each LFN entry holds up to 13 characters. They are stored in reverse
 * order (highest sequence number first).
 * ======================================================================== */

#define LFN_MAX_ENTRIES  20   /* Max LFN entries = 20 * 13 = 260 chars */
#define LFN_MAX_CHARS   255

/* Extract UCS-2 characters from a LFN directory entry.
 * Each LFN entry stores 13 UCS-2 characters at fixed offsets:
 *   chars 0-4:  bytes 1-10   (5 chars)
 *   chars 5-10: bytes 14-25  (6 chars)
 *   chars 11-12: bytes 28-31 (2 chars)
 * A 0x0000 or 0xFFFF character marks the end of the name. */
static int lfn_extract_chars(const fat32_dir_entry_t *entry, uint16_t *chars)
{
    int count = 0;
    const uint8_t *raw = (const uint8_t *)entry;

    /* Characters 0-4: bytes 1-10 */
    for (int i = 0; i < 5; i++) {
        uint16_t ch = raw[1 + i * 2] | ((uint16_t)raw[2 + i * 2] << 8);
        if (ch == 0x0000 || ch == 0xFFFF) return count;
        chars[count++] = ch;
    }
    /* Characters 5-10: bytes 14-25 */
    for (int i = 0; i < 6; i++) {
        uint16_t ch = raw[14 + i * 2] | ((uint16_t)raw[15 + i * 2] << 8);
        if (ch == 0x0000 || ch == 0xFFFF) return count;
        chars[count++] = ch;
    }
    /* Characters 11-12: bytes 28-31 */
    for (int i = 0; i < 2; i++) {
        uint16_t ch = raw[28 + i * 2] | ((uint16_t)raw[29 + i * 2] << 8);
        if (ch == 0x0000 || ch == 0xFFFF) return count;
        chars[count++] = ch;
    }
    return count;
}

static int fat32_populate_dir(vfs_superblock_t *sb, vfs_dentry_t *parent,
                              uint32_t dir_cluster)
{
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)sb->fs_data;
    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -1;

    /* LFN accumulation state */
    uint16_t lfn_chars[LFN_MAX_CHARS];
    int      lfn_char_count = 0;
    int      lfn_seq_expected = 0;  /* Expected next sequence number (descending) */

    /* Collect all entries across all clusters first, then process.
     * This simplifies LFN handling across cluster boundaries. */
    /* For simplicity, we process cluster-by-cluster but carry LFN state across. */

    uint32_t cluster = dir_cluster;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            free_page(cluster_buf);
            return -2;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t first_byte = entries[i].name[0];

            /* End of directory */
            if (first_byte == 0x00) {
                free_page(cluster_buf);
                return 0;
            }

            /* Deleted entry - skip and reset LFN state */
            if (first_byte == 0xE5) {
                lfn_char_count = 0;
                lfn_seq_expected = 0;
                continue;
            }

            /* Handle LFN entries */
            if (entries[i].attr == FAT32_ATTR_LFN) {
                uint8_t seq = entries[i].name[0];
                int is_last = (seq & 0x40) != 0;
                seq &= 0x3F;  /* Mask off the last-entry bit */

                if (is_last) {
                    /* This is the first LFN entry (highest sequence number) */
                    lfn_char_count = 0;
                    lfn_seq_expected = seq;

                    /* Extract characters and place them at the correct position */
                    uint16_t chars[13];
                    int nchars = lfn_extract_chars(&entries[i], chars);

                    /* Calculate starting position for this segment */
                    int start_pos = (seq - 1) * 13;
                    if (start_pos + nchars <= LFN_MAX_CHARS) {
                        for (int j = 0; j < nchars; j++) {
                            lfn_chars[start_pos + j] = chars[j];
                        }
                        lfn_char_count = start_pos + nchars;
                    }
                    lfn_seq_expected = seq - 1;
                } else if (seq == lfn_seq_expected && lfn_seq_expected > 0) {
                    /* Continuing LFN sequence */
                    uint16_t chars[13];
                    int nchars = lfn_extract_chars(&entries[i], chars);

                    int start_pos = (seq - 1) * 13;
                    if (start_pos + nchars <= LFN_MAX_CHARS) {
                        for (int j = 0; j < nchars; j++) {
                            lfn_chars[start_pos + j] = chars[j];
                        }
                        /* Don't update lfn_char_count for middle entries;
                         * it was set by the first (last-seq) entry */
                    }
                    lfn_seq_expected = seq - 1;
                } else {
                    /* Out-of-sequence LFN, reset */
                    lfn_char_count = 0;
                    lfn_seq_expected = 0;
                }
                continue;
            }

            /* Skip volume ID */
            if (entries[i].attr & FAT32_ATTR_VOLUME_ID) {
                lfn_char_count = 0;
                lfn_seq_expected = 0;
                continue;
            }

            /* This is a normal 8.3 entry - determine the name */
            char name[VFS_MAX_NAME];

            if (lfn_char_count > 0 && lfn_seq_expected == 0) {
                /* We have a complete LFN - convert UCS-2 to ASCII/UTF-8 */
                int pos = 0;
                for (int j = 0; j < lfn_char_count && pos < VFS_MAX_NAME - 1; j++) {
                    if (lfn_chars[j] < 0x80) {
                        name[pos++] = (char)lfn_chars[j];
                    } else {
                        /* Non-ASCII: use '?' placeholder */
                        name[pos++] = '?';
                    }
                }
                name[pos] = '\0';
            } else {
                /* No valid LFN, use 8.3 name */
                fat32_format_83_name(entries[i].name, name, VFS_MAX_NAME);
            }

            /* Reset LFN state */
            lfn_char_count = 0;
            lfn_seq_expected = 0;

            /* Skip "." and ".." */
            if (name[0] == '.' && (name[1] == '\0' ||
                (name[1] == '.' && name[2] == '\0')))
                continue;

            /* Get starting cluster */
            uint32_t entry_cluster = (uint32_t)entries[i].cluster_hi << 16 | entries[i].cluster_lo;
            int is_dir = (entries[i].attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;

            /* Check if dentry already exists */
            vfs_dentry_t *existing = NULL;
            vfs_dentry_t *child = parent->child;
            while (child) {
                if (strcmp(child->name, name) == 0) {
                    existing = child;
                    break;
                }
                child = child->next;
            }
            if (existing)
                continue;

            /* Create inode */
            vfs_inode_t *new_inode = (vfs_inode_t *)alloc_page();
            if (!new_inode) continue;
            memset(new_inode, 0, PAGE_SIZE);

            new_inode->ino = fat32_next_ino++;
            new_inode->type = is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            new_inode->mode = VFS_S_IRUSR | VFS_S_IRGRP | VFS_S_IROTH;
            if (is_dir)
                new_inode->mode |= VFS_S_IXUSR | VFS_S_IXGRP | VFS_S_IXOTH;
            new_inode->size = is_dir ? 0 : entries[i].file_size;
            new_inode->nlinks = 1;
            new_inode->sb = sb;

            /* Allocate inode private data */
            fat32_inode_data_t *inode_data = (fat32_inode_data_t *)alloc_page();
            if (!inode_data) {
                free_page(new_inode);
                continue;
            }
            memset(inode_data, 0, PAGE_SIZE);
            inode_data->start_cluster = entry_cluster;
            inode_data->file_size = entries[i].file_size;
            inode_data->parent_cluster = dir_cluster;
            memcpy(inode_data->name83, entries[i].name, 11);
            new_inode->fs_data = inode_data;

            /* Set read/write operations for files */
            if (!is_dir) {
                new_inode->read = fat32_inode_read;
                new_inode->write = fat32_inode_write;
            }

            /* Create dentry */
            vfs_dentry_t *new_dentry = (vfs_dentry_t *)alloc_page();
            if (!new_dentry) {
                free_page(inode_data);
                free_page(new_inode);
                continue;
            }
            memset(new_dentry, 0, PAGE_SIZE);
            strncpy(new_dentry->name, name, VFS_MAX_NAME - 1);
            new_dentry->inode = new_inode;
            new_dentry->parent = parent;

            /* Add to parent's child list */
            new_dentry->next = parent->child;
            parent->child = new_dentry;
        }

        /* Follow cluster chain */
        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    free_page(cluster_buf);
    return 0;
}

/* ========================================================================
 * Recursive directory tree population
 *
 * Walks the FAT32 directory tree and creates corresponding VFS dentries.
 * ======================================================================== */

static void fat32_populate_recursive(vfs_superblock_t *sb, vfs_dentry_t *parent,
                                     uint32_t dir_cluster)
{
    /* First, populate this directory */
    fat32_populate_dir(sb, parent, dir_cluster);

    /* Then recurse into subdirectories */
    vfs_dentry_t *child = parent->child;
    while (child) {
        if (child->inode && child->inode->type == VFS_TYPE_DIR && child->inode->fs_data) {
            fat32_inode_data_t *id = (fat32_inode_data_t *)child->inode->fs_data;
            fat32_populate_recursive(sb, child, id->start_cluster);
        }
        child = child->next;
    }
}

/* ========================================================================
 * FAT32 filesystem operations
 * ======================================================================== */

static int fat32_mount(vfs_superblock_t *sb, uint8_t blkdev_id, const char *options)
{
    /* Parse block device ID from options string if provided.
     * Format: "blkdev=N" */
    uint8_t dev_id = blkdev_id;

    if (options) {
        const char *p = options;
        while (*p) {
            if (strncmp(p, "blkdev=", 7) == 0) {
                p += 7;
                int val = 0;
                while (*p >= '0' && *p <= '9') {
                    val = val * 10 + (*p - '0');
                    p++;
                }
                dev_id = (uint8_t)val;
                break;
            }
            p++;
        }
    }

    if (dev_id == 0xFF) {
        printf("[FAT32] No block device specified\n");
        return -1;
    }

    /* Read BPB from sector 0 */
    void *sector_buf = alloc_page();
    if (!sector_buf)
        return -2;

    int ret = blkdev_read(dev_id, 0, 1, sector_buf);
    if (ret != 0) {
        printf("[FAT32] Failed to read BPB (err=%d)\n", ret);
        free_page(sector_buf);
        return -3;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector_buf;

    /* Validate FAT32 signature */
    if (bpb->signature != 0xAA55) {
        printf("[FAT32] Invalid boot signature: 0x%04X\n", bpb->signature);
        free_page(sector_buf);
        return -4;
    }

    /* Verify this is actually FAT32 */
    if (bpb->bytes_per_sector == 0 ||
        (bpb->bytes_per_sector & (bpb->bytes_per_sector - 1)) != 0) {
        printf("[FAT32] Invalid bytes_per_sector: %u\n", bpb->bytes_per_sector);
        free_page(sector_buf);
        return -5;
    }

    if (bpb->sectors_per_fat_32 == 0) {
        printf("[FAT32] Not a FAT32 filesystem (sectors_per_fat_32 = 0)\n");
        free_page(sector_buf);
        return -6;
    }

    /* Check for "FAT32" in fs_type field */
    if (memcmp(bpb->fs_type, "FAT32   ", 8) != 0) {
        printf("[FAT32] fs_type mismatch: %.8s\n", bpb->fs_type);
        /* Some FAT32 images may not have this set correctly, continue anyway */
    }

    /* Allocate superblock private data */
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)alloc_page();
    if (!sb_data) {
        free_page(sector_buf);
        return -7;
    }
    memset(sb_data, 0, PAGE_SIZE);

    sb_data->blkdev_id = dev_id;
    sb_data->bytes_per_sector = bpb->bytes_per_sector;
    sb_data->sectors_per_cluster = bpb->sectors_per_cluster;
    sb_data->reserved_sectors = bpb->reserved_sectors;
    sb_data->sectors_per_fat = bpb->sectors_per_fat_32;
    sb_data->root_dir_cluster = bpb->root_dir_cluster;

    /* Calculate FAT start sector */
    sb_data->fat_start_sector = bpb->reserved_sectors;

    /* Calculate data start sector */
    sb_data->data_start_sector = bpb->reserved_sectors +
                                  (uint32_t)bpb->num_fats * bpb->sectors_per_fat_32;

    /* Calculate total clusters */
    uint32_t total_sectors = bpb->total_sectors_32;
    if (total_sectors == 0)
        total_sectors = bpb->total_sectors_16;

    uint32_t data_sectors = total_sectors - sb_data->data_start_sector;
    sb_data->total_clusters = data_sectors / sb_data->sectors_per_cluster;
    sb_data->total_data_clusters = sb_data->total_clusters;
    sb_data->next_free_cluster = 2;

    sb->fs_data = sb_data;
    sb->blkdev_id = dev_id;

    printf("[FAT32] Mounted on blkdev %u, bytes/sector=%u, sectors/cluster=%u\n",
           dev_id, sb_data->bytes_per_sector, sb_data->sectors_per_cluster);
    printf("[FAT32] FAT start=%u, data start=%u, root cluster=%u, total clusters=%u\n",
           sb_data->fat_start_sector, sb_data->data_start_sector,
           sb_data->root_dir_cluster, sb_data->total_clusters);

    /* Create root inode */
    vfs_inode_t *root = (vfs_inode_t *)alloc_page();
    if (!root) {
        free_page(sb_data);
        free_page(sector_buf);
        return -8;
    }
    memset(root, 0, PAGE_SIZE);
    root->ino = fat32_next_ino++;
    root->type = VFS_TYPE_DIR;
    root->mode = VFS_S_IRUSR | VFS_S_IXUSR | VFS_S_IRGRP | VFS_S_IXGRP |
                 VFS_S_IROTH | VFS_S_IXOTH;
    root->size = 0;
    root->nlinks = 2;
    root->sb = sb;

    /* Allocate root inode private data */
    fat32_inode_data_t *root_data = (fat32_inode_data_t *)alloc_page();
    if (!root_data) {
        free_page(root);
        free_page(sb_data);
        free_page(sector_buf);
        return -9;
    }
    memset(root_data, 0, PAGE_SIZE);
    root_data->start_cluster = sb_data->root_dir_cluster;
    root_data->file_size = 0;
    root->fs_data = root_data;

    /* Create root dentry */
    vfs_dentry_t *root_dentry = (vfs_dentry_t *)alloc_page();
    if (!root_dentry) {
        free_page(root_data);
        free_page(root);
        free_page(sb_data);
        free_page(sector_buf);
        return -10;
    }
    memset(root_dentry, 0, PAGE_SIZE);
    strcpy(root_dentry->name, "/");
    root_dentry->inode = root;
    root_dentry->parent = root_dentry;

    sb->root = root_dentry;
    sb->root_inode = root;

    free_page(sector_buf);

    /* Populate the directory tree */
    fat32_populate_recursive(sb, root_dentry, sb_data->root_dir_cluster);

    return 0;
}

static int fat32_unmount(vfs_superblock_t *sb)
{
    if (sb->fs_data) {
        free_page(sb->fs_data);
        sb->fs_data = NULL;
    }
    return 0;
}

static vfs_inode_t *fat32_alloc_inode(vfs_superblock_t *sb)
{
    vfs_inode_t *inode = (vfs_inode_t *)alloc_page();
    if (!inode) return NULL;
    memset(inode, 0, PAGE_SIZE);

    inode->ino = fat32_next_ino++;
    inode->sb = sb;

    /* Allocate inode private data */
    fat32_inode_data_t *data = (fat32_inode_data_t *)alloc_page();
    if (!data) {
        free_page(inode);
        return NULL;
    }
    memset(data, 0, PAGE_SIZE);
    inode->fs_data = data;

    return inode;
}

static void fat32_destroy_inode(vfs_inode_t *inode)
{
    if (inode->fs_data) {
        free_page(inode->fs_data);
        inode->fs_data = NULL;
    }
    free_page(inode);
}

/* ========================================================================
 * Filesystem type definition
 * ======================================================================== */

vfs_filesystem_t fat32_fs = {
    .name = "fat32",
    .mount = fat32_mount,
    .unmount = fat32_unmount,
    .alloc_inode = fat32_alloc_inode,
    .destroy_inode = fat32_destroy_inode,
    .create = fat32_create,
    .mkdir = fat32_mkdir_cb,
    .unlink = fat32_unlink_cb,
};

/* ========================================================================
 * Initialization
 * ======================================================================== */

void fat32_init(void)
{
    vfs_register_fs(&fat32_fs);
}
