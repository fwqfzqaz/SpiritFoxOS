/* SpiritFoxOS - SpiritFox文件系统接口
 * Copyright (C) 2025 SpiritFoxOS Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef SFS_H
#define SFS_H

#include <stdint.h>

#define SFS_MAGIC       0x53465331  /* "SFS1" */
#define SFS_VERSION     1
#define SFS_MAX_FILES   64
#define SFS_FILENAME_MAX 32
#define SFS_START_LBA   2048        /* Start after ISO area */
#define SFS_RESERVED_SECS 2         /* Superblock + bitmap */

typedef struct {
    char     name[SFS_FILENAME_MAX];
    uint32_t size;
    uint32_t start_sector;          /* Relative to data area start */
    uint32_t create_time;
    uint32_t modify_time;
    uint8_t  flags;                 /* 0=normal, 1=deleted */
    uint8_t  reserved[3];
    uint32_t checksum;
} sfs_file_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t total_sectors;
    uint32_t file_table_sector;     /* LBA of file table */
    uint32_t file_table_count;
    uint32_t data_area_sector;      /* LBA of data area */
    uint32_t sector_count;          /* Sectors in data area */
    uint8_t  reserved[484];
    uint32_t checksum;
} __attribute__((packed)) sfs_superblock_t;

void sfs_init(void);
int sfs_format(void);
int sfs_create_file(const char *name);
int sfs_write_file(const char *name, const void *data, uint32_t size);
int sfs_read_file(const char *name, void *buf, uint32_t buf_size, uint32_t *out_size);
int sfs_delete_file(const char *name);
int sfs_list_files(sfs_file_entry_t *entries, int max_entries);
int sfs_file_exists(const char *name);
int sfs_get_file_size(const char *name, uint32_t *size);
int sfs_is_formatted(void);

#endif
