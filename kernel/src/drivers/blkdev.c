#include "blkdev.h"
#include "string.h"
#include "vga.h"

static blkdev_t devices[BLKDEV_MAX_DEVICES];

void blkdev_init(void)
{
    for (int i = 0; i < BLKDEV_MAX_DEVICES; i++) {
        devices[i].id = i;
        devices[i].in_use = 0;
        devices[i].type = BLKDEV_TYPE_NONE;
        devices[i].sector_count = 0;
        devices[i].sector_size = 0;
        devices[i].name[0] = '\0';
        devices[i].ops.read = NULL;
        devices[i].ops.write = NULL;
        devices[i].driver_data = NULL;
    }
}

int blkdev_register(blkdev_type_t type, uint64_t sector_count,
                    uint32_t sector_size, const char *name,
                    blkdev_ops_t *ops, void *driver_data)
{
    for (int i = 0; i < BLKDEV_MAX_DEVICES; i++) {
        if (!devices[i].in_use) {
            devices[i].in_use = 1;
            devices[i].type = type;
            devices[i].sector_count = sector_count;
            devices[i].sector_size = sector_size;
            strncpy(devices[i].name, name, sizeof(devices[i].name) - 1);
            devices[i].name[sizeof(devices[i].name) - 1] = '\0';
            if (ops) {
                devices[i].ops = *ops;
            }
            devices[i].driver_data = driver_data;
            return i;
        }
    }
    return -1;
}

void blkdev_unregister(uint8_t dev_id)
{
    if (dev_id < BLKDEV_MAX_DEVICES) {
        devices[dev_id].in_use = 0;
        devices[dev_id].type = BLKDEV_TYPE_NONE;
    }
}

blkdev_t *blkdev_get(uint8_t dev_id)
{
    if (dev_id < BLKDEV_MAX_DEVICES && devices[dev_id].in_use) {
        return &devices[dev_id];
    }
    return NULL;
}

int blkdev_read(uint8_t dev_id, uint64_t lba, uint32_t count, void *buf)
{
    if (dev_id >= BLKDEV_MAX_DEVICES || !devices[dev_id].in_use)
        return -1;
    if (!devices[dev_id].ops.read)
        return -2;
    return devices[dev_id].ops.read(dev_id, lba, count, buf);
}

int blkdev_write(uint8_t dev_id, uint64_t lba, uint32_t count, const void *buf)
{
    if (dev_id >= BLKDEV_MAX_DEVICES || !devices[dev_id].in_use)
        return -1;
    if (!devices[dev_id].ops.write)
        return -2;
    return devices[dev_id].ops.write(dev_id, lba, count, buf);
}

static const char *blkdev_type_name(blkdev_type_t type)
{
    switch (type) {
    case BLKDEV_TYPE_AHCI:    return "AHCI";
    case BLKDEV_TYPE_RAMDISK: return "RAMDisk";
    case BLKDEV_TYPE_IDE:     return "IDE";
    default:                  return "Unknown";
    }
}

void blkdev_list(void)
{
    printf("Block Devices:\n");
    printf("  ID  Name     Type     Sectors    Size\n");
    for (int i = 0; i < BLKDEV_MAX_DEVICES; i++) {
        if (devices[i].in_use) {
            uint64_t size_kb = devices[i].sector_count * devices[i].sector_size / 1024;
            printf("  %d   %-8s %-8s %llu %lluKB\n",
                   i, devices[i].name,
                   blkdev_type_name(devices[i].type),
                   devices[i].sector_count, size_kb);
        }
    }
}

int blkdev_count(void)
{
    int count = 0;
    for (int i = 0; i < BLKDEV_MAX_DEVICES; i++) {
        if (devices[i].in_use)
            count++;
    }
    return count;
}
