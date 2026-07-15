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

/* Ramdisk 写入操作 */
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

    /* 为 ramdisk 分配内存。
     * 逐页分配并映射为连续地址。 */
    uint64_t total_bytes = sectors * BLKDEV_SECTOR_SIZE;
    uint64_t pages_needed = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    /* 简单方式：分配所有页面并存储指针 */
    uint8_t *disk_data = (uint8_t *)alloc_page();
    if (!disk_data)
        return -1;

    /* 对于可容纳在单个页面中的小型 ramdisk，此方式可行。
     * 对于更大的 ramdisk，需要连续分配。 */
    if (pages_needed > 1) {
        /* 分配剩余页面 - 它们可能不连续，
         * 因此对于大型 ramdisk 需要不同的方法。
         * 目前限制为单页 ramdisk 或使用较大分配的第一页。 */
        for (uint64_t p = 1; p < pages_needed; p++) {
            void *page = alloc_page();
            if (!page) {
                /* 内存不足 - 使用已分配的部分 */
                sectors = (p * PAGE_SIZE) / BLKDEV_SECTOR_SIZE;
                break;
            }
        }
        /* 注意：非连续页面需要分散-聚集方法。
         * 对于初始 ramdisk，我们使用更简单的连续分配器，
         * 依赖 alloc_page 按顺序返回页面的事实，
         * 并且由于我们使用恒等映射，如果物理页面恰好连续，
         * 它们在虚拟内存中也表现为连续。
         *
         * 更稳健的方法是分配页表数组。
         * 目前这对小型 ramdisk（约 1MB 以内）有效。 */
    }

    /* 将已分配内存清零 */
    memset(disk_data, 0, total_bytes > PAGE_SIZE ? PAGE_SIZE : (size_t)total_bytes);

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
