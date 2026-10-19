#include "ramdisk.h"
#include "blkdev.h"
#include "memory.h"
#include "string.h"
#include "vga.h"

/* 每个 ramdisk 的私有数据 */
typedef struct {
    uint8_t  *data;       /* 已分配内存的指针 */
    uint64_t  sectors;    /* 扇区数量 */
} ramdisk_priv_t;

static ramdisk_priv_t ramdisks[RAMDISK_MAX_DISKS];

/* Ramdisk 读取操作 */
static int ramdisk_read(uint8_t dev_id, uint64_t lba, uint32_t count, void *buf)
{
    blkdev_t *dev = blkdev_get(dev_id);
    if (!dev || !dev->driver_data)
        return -1;

    ramdisk_priv_t *priv = (ramdisk_priv_t *)dev->driver_data;

    if (lba > priv->sectors || count > priv->sectors - lba || (count && !buf))
        return -2;
    if (!count) return 0;

    uint64_t offset = lba * BLKDEV_SECTOR_SIZE;
    uint64_t size = (uint64_t)count * BLKDEV_SECTOR_SIZE;
    memcpy(buf, priv->data + offset, size);
    return 0;
}

/* Ramdisk 写入操作 */
static int ramdisk_write(uint8_t dev_id, uint64_t lba, uint32_t count, const void *buf)
{
    blkdev_t *dev = blkdev_get(dev_id);
    if (!dev || !dev->driver_data)
        return -1;

    ramdisk_priv_t *priv = (ramdisk_priv_t *)dev->driver_data;

    if (lba > priv->sectors || count > priv->sectors - lba || (count && !buf))
        return -2;
    if (!count) return 0;

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
    /* The rounded byte extent must fit size_t before allocation or clearing. */
    if (!sectors || sectors > (SIZE_MAX - (PAGE_SIZE - 1)) / BLKDEV_SECTOR_SIZE)
        return -1;
    /* 查找空闲的 ramdisk 槽位 */
    int slot = -1;
    for (int i = 0; i < RAMDISK_MAX_DISKS; i++) {
        if (ramdisks[i].data == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    /* One contiguous extent; a fragmented/OOM allocation must fail atomically. */
    uint64_t total_bytes = sectors * BLKDEV_SECTOR_SIZE;
    uint64_t pages_needed = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint8_t *disk_data = (uint8_t *)alloc_pages((size_t)pages_needed);
    if (!disk_data)
        return -1;

    memset(disk_data, 0, (size_t)pages_needed * PAGE_SIZE);

    ramdisks[slot].data = disk_data;
    ramdisks[slot].sectors = sectors;

    /* 注册为块设备 */
    char name[16];
    name[0] = 'r'; name[1] = 'a'; name[2] = 'm'; name[3] = '0' + slot;
    name[4] = '\0';

    int dev_id = blkdev_register(BLKDEV_TYPE_RAMDISK, sectors,
                                 BLKDEV_SECTOR_SIZE, name,
                                 &ramdisk_ops, &ramdisks[slot]);
    if (dev_id < 0) {
        for (uint64_t page = 0; page < pages_needed; page++)
            free_page(disk_data + page * PAGE_SIZE);
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

int ramdisk_create_from_buffer(void *data, uint64_t size_bytes)
{
    if (!data || size_bytes < BLKDEV_SECTOR_SIZE ||
        size_bytes - 1 > UINTPTR_MAX - (uintptr_t)data) return -1;
    /* 查找空闲的 ramdisk 槽位 */
    int slot = -1;
    for (int i = 0; i < RAMDISK_MAX_DISKS; i++) {
        if (ramdisks[i].data == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    uint64_t sectors = size_bytes / BLKDEV_SECTOR_SIZE;

    ramdisks[slot].data = (uint8_t *)data;
    ramdisks[slot].sectors = sectors;

    /* 注册为块设备 */
    char name[16];
    name[0] = 'i'; name[1] = 'n'; name[2] = 'i'; name[3] = 't'; name[4] = 'r'; name[5] = 'd'; name[6] = '\0';

    int dev_id = blkdev_register(BLKDEV_TYPE_RAMDISK, sectors,
                                 BLKDEV_SECTOR_SIZE, name,
                                 &ramdisk_ops, &ramdisks[slot]);
    if (dev_id < 0) {
        ramdisks[slot].data = NULL;
        ramdisks[slot].sectors = 0;
        return -1;
    }

    printf("[RAMDISK] initrd: %llu sectors (%llu KB) at 0x%llx\n",
           sectors, sectors * BLKDEV_SECTOR_SIZE / 1024, (uint64_t)(uintptr_t)data);

    return dev_id;
}
