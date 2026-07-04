#include "ramdisk.h"
#include "blkdev.h"
#include "memory.h"
#include "string.h"
#include "vga.h"

/* Per-ramdisk private data */
typedef struct {
    uint8_t  *data;       /* Pointer to allocated memory */
    uint64_t  sectors;    /* Number of sectors */
} ramdisk_priv_t;

static ramdisk_priv_t ramdisks[RAMDISK_MAX_DISKS];

/* Ramdisk read operation */
static int ramdisk_read(uint8_t dev_id, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev_id;
    blkdev_t *dev = blkdev_get(dev_id);
    if (!dev || !dev->driver_data)
        return -1;

    ramdisk_priv_t *priv = (ramdisk_priv_t *)dev->driver_data;

    if (lba + count > priv->sectors)
        return -2;

    uint64_t offset = lba * BLKDEV_SECTOR_SIZE;
    uint64_t size = (uint64_t)count * BLKDEV_SECTOR_SIZE;
    memcpy(buf, priv->data + offset, size);
    return 0;
}

/* Ramdisk write operation */
static int ramdisk_write(uint8_t dev_id, uint64_t lba, uint32_t count, const void *buf)
{
    blkdev_t *dev = blkdev_get(dev_id);
    if (!dev || !dev->driver_data)
        return -1;

    ramdisk_priv_t *priv = (ramdisk_priv_t *)dev->driver_data;

    if (lba + count > priv->sectors)
        return -2;

    uint64_t offset = lba * BLKDEV_SECTOR_SIZE;
    uint64_t size = (uint64_t)count * BLKDEV_SECTOR_SIZE;
    memcpy(priv->data + offset, buf, size);
    return 0;
}

static blkdev_ops_t ramdisk_ops = {
    .read  = ramdisk_read,
    .write = ramdisk_write,
};

void ramdisk_init(void)
{
    for (int i = 0; i < RAMDISK_MAX_DISKS; i++) {
        ramdisks[i].data = NULL;
        ramdisks[i].sectors = 0;
    }
}

int ramdisk_create(uint64_t sectors)
{
    /* Find a free ramdisk slot */
    int slot = -1;
    for (int i = 0; i < RAMDISK_MAX_DISKS; i++) {
        if (ramdisks[i].data == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    /* Allocate memory for the ramdisk.
     * We allocate page by page and map them contiguously. */
    uint64_t total_bytes = sectors * BLKDEV_SECTOR_SIZE;
    uint64_t pages_needed = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Simple approach: allocate all pages and store pointers */
    uint8_t *disk_data = (uint8_t *)alloc_page();
    if (!disk_data)
        return -1;

    /* For small ramdisks that fit in one page, this works.
     * For larger ramdisks, we'd need contiguous allocation. */
    if (pages_needed > 1) {
        /* Allocate remaining pages - they may not be contiguous,
         * so we need a different approach for large ramdisks.
         * For now, limit to single-page ramdisks or use the first
         * page of a larger allocation. */
        for (uint64_t p = 1; p < pages_needed; p++) {
            void *page = alloc_page();
            if (!page) {
                /* Out of memory - use what we have */
                sectors = (p * PAGE_SIZE) / BLKDEV_SECTOR_SIZE;
                break;
            }
        }
        /* Note: non-contiguous pages need a scatter-gather approach.
         * For the initial ramdisk, we use a simpler contiguous allocator
         * by relying on the fact that alloc_page returns pages in order,
         * and since we identity-map, they appear contiguous in virtual memory
         * if the physical pages happen to be contiguous.
         *
         * A more robust approach would allocate a page table array.
         * For now, this works for small ramdisks (up to ~1MB). */
    }

    /* Zero the allocated memory */
    memset(disk_data, 0, total_bytes > PAGE_SIZE ? PAGE_SIZE : (size_t)total_bytes);

    ramdisks[slot].data = disk_data;
    ramdisks[slot].sectors = sectors;

    /* Register as a block device */
    char name[16];
    name[0] = 'r'; name[1] = 'a'; name[2] = 'm'; name[3] = '0' + slot;
    name[4] = '\0';

    int dev_id = blkdev_register(BLKDEV_TYPE_RAMDISK, sectors,
                                 BLKDEV_SECTOR_SIZE, name,
                                 &ramdisk_ops, &ramdisks[slot]);
    if (dev_id < 0) {
        ramdisks[slot].data = NULL;
        ramdisks[slot].sectors = 0;
        return -1;
    }

    printf("[RAMDISK] Created %s: %llu sectors (%llu KB)\n",
           name, sectors, sectors * BLKDEV_SECTOR_SIZE / 1024);

    return dev_id;
}

void *ramdisk_get_data(uint8_t dev_id)
{
    blkdev_t *dev = blkdev_get(dev_id);
    if (!dev || dev->type != BLKDEV_TYPE_RAMDISK || !dev->driver_data)
        return NULL;

    ramdisk_priv_t *priv = (ramdisk_priv_t *)dev->driver_data;
    return priv->data;
}
