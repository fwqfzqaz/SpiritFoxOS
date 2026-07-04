#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>
#include <stddef.h>

#define BLKDEV_MAX_DEVICES  16
#define BLKDEV_SECTOR_SIZE  512

typedef enum {
    BLKDEV_TYPE_NONE = 0,
    BLKDEV_TYPE_AHCI,
    BLKDEV_TYPE_RAMDISK,
    BLKDEV_TYPE_IDE,
} blkdev_type_t;

/* Block device operations - each driver implements these */
typedef struct blkdev_ops {
    /* Read sectors into buffer. Returns 0 on success, negative on error */
    int (*read)(uint8_t dev_id, uint64_t lba, uint32_t count, void *buf);
    /* Write sectors from buffer. Returns 0 on success, negative on error */
    int (*write)(uint8_t dev_id, uint64_t lba, uint32_t count, const void *buf);
} blkdev_ops_t;

/* Block device descriptor */
typedef struct blkdev {
    uint8_t         id;         /* Device index (0-based) */
    uint8_t         in_use;     /* 1 if slot is occupied */
    blkdev_type_t   type;       /* Device type */
    uint64_t        sector_count; /* Total number of sectors */
    uint32_t        sector_size;  /* Sector size in bytes (usually 512) */
    char            name[16];     /* Device name (e.g., "sda", "ram0") */
    blkdev_ops_t    ops;        /* Device operations */
    void           *driver_data; /* Driver-specific private data */
} blkdev_t;

/* Initialize block device subsystem */
void blkdev_init(void);

/* Register a block device, returns assigned device ID or -1 on failure */
int blkdev_register(blkdev_type_t type, uint64_t sector_count,
                    uint32_t sector_size, const char *name,
                    blkdev_ops_t *ops, void *driver_data);

/* Unregister a block device */
void blkdev_unregister(uint8_t dev_id);

/* Get block device by ID */
blkdev_t *blkdev_get(uint8_t dev_id);

/* Read sectors from a block device */
int blkdev_read(uint8_t dev_id, uint64_t lba, uint32_t count, void *buf);

/* Write sectors to a block device */
int blkdev_write(uint8_t dev_id, uint64_t lba, uint32_t count, const void *buf);

/* List all registered block devices */
void blkdev_list(void);

/* Get the number of registered block devices */
int blkdev_count(void);

#endif /* BLKDEV_H */
