#ifndef RAMDISK_H
#define RAMDISK_H

#include <stdint.h>

#define RAMDISK_MAX_DISKS   4
#define RAMDISK_DEFAULT_SECTORS 2048  /* 1MB default (2048 * 512) */

/* Initialize ramdisk subsystem */
void ramdisk_init(void);

/* Create a zeroed ramdisk with the exact requested nonzero sector count.
 * Allocation is contiguous and all-or-nothing; no partial-capacity fallback.
 * Returns block device ID, or -1 on failure. */
int ramdisk_create(uint64_t sectors);

/* Get ramdisk data pointer for a given block device ID */
void *ramdisk_get_data(uint8_t dev_id);

/* Create a ramdisk backed by an existing memory buffer (e.g., initrd).
 * The buffer must remain valid for the lifetime of the ramdisk.
 * Only complete 512-byte sectors are exposed; registration never takes ownership.
 * Returns block device ID, or -1 on failure. */
int ramdisk_create_from_buffer(void *data, uint64_t size_bytes);

#endif /* RAMDISK_H */
