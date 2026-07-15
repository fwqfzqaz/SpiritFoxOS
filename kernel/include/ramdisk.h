#ifndef RAMDISK_H
#define RAMDISK_H

#include <stdint.h>

#define RAMDISK_MAX_DISKS   4
#define RAMDISK_DEFAULT_SECTORS 2048  /* 1MB default (2048 * 512) */

/* Initialize ramdisk subsystem */
void ramdisk_init(void);

/* Create a ramdisk with specified number of sectors.
 * Returns block device ID, or -1 on failure. */
int ramdisk_create(uint64_t sectors);

/* Get ramdisk data pointer for a given block device ID */
void *ramdisk_get_data(uint8_t dev_id);

#endif /* RAMDISK_H */
