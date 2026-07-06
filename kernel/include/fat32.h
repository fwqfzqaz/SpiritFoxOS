#ifndef FAT32_H
#define FAT32_H

#include "vfs.h"
#include <stdint.h>

/* ========================================================================
 * FAT32 Attribute Constants
 * ======================================================================== */

#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LFN        0x0F  /* (READ_ONLY | HIDDEN | SYSTEM | VOLUME_ID) */

/* End-of-chain marker */
#define FAT32_EOC_MIN         0x0FFFFFF8

/* ========================================================================
 * FAT32 BPB (BIOS Parameter Block) - from sector 0
 * ======================================================================== */

typedef struct {
    uint8_t  jmp_boot[3];        /* Jump instruction */
    char     oem_name[8];        /* OEM name */
    uint16_t bytes_per_sector;   /* Bytes per sector (512, 1024, 2048, 4096) */
    uint8_t  sectors_per_cluster;/* Sectors per cluster (power of 2) */
    uint16_t reserved_sectors;   /* Reserved sectors before FAT */
    uint8_t  num_fats;           /* Number of FAT tables (usually 2) */
    uint16_t root_entry_count;   /* Must be 0 for FAT32 */
    uint16_t total_sectors_16;   /* Must be 0 for FAT32 */
    uint8_t  media_type;         /* Media type (0xF8 = hard disk) */
    uint16_t sectors_per_fat_16; /* Must be 0 for FAT32 */
    uint16_t sectors_per_track;  /* Sectors per track */
    uint16_t num_heads;          /* Number of heads */
    uint32_t hidden_sectors;     /* Hidden sectors */
    uint32_t total_sectors_32;   /* Total sectors (if total_sectors_16 == 0) */

    /* FAT32 extended BPB */
    uint32_t sectors_per_fat_32; /* Sectors per FAT table */
    uint16_t ext_flags;          /* Extended flags */
    uint16_t fs_version;         /* Filesystem version (0) */
    uint32_t root_dir_cluster;   /* First cluster of root directory */
    uint16_t fs_info_sector;     /* Sector number of FSINFO */
    uint16_t backup_boot_sector; /* Sector number of backup boot sector */
    uint8_t  reserved[12];       /* Reserved */
    uint8_t  drive_number;       /* Drive number */
    uint8_t  reserved1;          /* Reserved */
    uint8_t  boot_sig;           /* Extended boot signature (0x29) */
    uint32_t volume_serial;      /* Volume serial number */
    char     volume_label[11];   /* Volume label */
    char     fs_type[8];         /* "FAT32   " */
    uint8_t  boot_code[420];     /* Boot code */
    uint16_t signature;          /* Boot signature (0xAA55) */
} __attribute__((packed)) fat32_bpb_t;

/* ========================================================================
 * FAT32 Directory Entry (32 bytes)
 * ======================================================================== */

typedef struct {
    uint8_t  name[8];            /* 8.3 filename (space-padded) */
    uint8_t  ext[3];             /* Extension (space-padded) */
    uint8_t  attr;               /* File attributes */
    uint8_t  nt_reserved;        /* Reserved for NT */
    uint8_t  create_time_tenth;  /* Creation time (tenths of second) */
    uint16_t create_time;        /* Creation time */
    uint16_t create_date;        /* Creation date */
    uint16_t access_date;        /* Last access date */
    uint16_t cluster_hi;         /* High 16 bits of starting cluster */
    uint16_t modify_time;        /* Modification time */
    uint16_t modify_date;        /* Modification date */
    uint16_t cluster_lo;         /* Low 16 bits of starting cluster */
    uint32_t file_size;          /* File size in bytes */
} __attribute__((packed)) fat32_dir_entry_t;

/* ========================================================================
 * FAT32 Superblock private data
 * ======================================================================== */

typedef struct {
    uint8_t  blkdev_id;          /* Block device ID */
    uint16_t bytes_per_sector;   /* Bytes per sector */
    uint8_t  sectors_per_cluster;/* Sectors per cluster */
    uint32_t reserved_sectors;   /* Reserved sectors before FAT */
    uint32_t sectors_per_fat;    /* Sectors per FAT table */
    uint32_t root_dir_cluster;   /* Root directory starting cluster */
    uint32_t fat_start_sector;   /* First sector of FAT table */
    uint32_t data_start_sector;  /* First sector of data area */
    uint32_t total_clusters;     /* Total data clusters */
    uint32_t next_free_cluster;  /* Hint for next free cluster search */
    uint32_t total_data_clusters;/* Total data area clusters */
} fat32_sb_data_t;

/* ========================================================================
 * FAT32 Inode private data
 * ======================================================================== */

typedef struct {
    uint32_t start_cluster;      /* Starting cluster number */
    uint32_t file_size;          /* File size in bytes */
    uint32_t parent_cluster;     /* Starting cluster of parent directory */
    uint8_t  name83[11];         /* 8.3 name in directory entry format */
} fat32_inode_data_t;

/* ========================================================================
 * FAT32 Filesystem type declaration
 * ======================================================================== */

extern vfs_filesystem_t fat32_fs;

/* Initialize and register fat32 filesystem */
void fat32_init(void);

#endif /* FAT32_H */
