#include "blkdev.h"
#include "string.h"
#include "vga.h"

static blkdev_t devices[BLKDEV_MAX_DEVICES];
static int sealed;

void blkdev_init(void)
{
    /* A boot snapshot cannot be invalidated by reinitialization or slot reuse. */
    if (sealed) return;
    for (int i = 0; i < BLKDEV_MAX_DEVICES; i++) {
        devices[i].id = i;
        devices[i].in_use = 0;
        devices[i].type = BLKDEV_TYPE_NONE;
        devices[i].sector_count = 0;
        devices[i].sector_size = 0;
        devices[i].geometry_source = BLKDEV_GEOMETRY_UNKNOWN;
        devices[i].name[0] = '\0';
        devices[i].ops.read = NULL;
        devices[i].ops.write = NULL;
        devices[i].driver_data = NULL;
    }
}

int blkdev_register(blkdev_type_t type, blkdev_geometry_t geometry_source, uint64_t sector_count,
                    uint32_t sector_size, const char *name,
                    blkdev_ops_t *ops, void *driver_data)
{
    if (sealed || !name || !name[0] || type < BLKDEV_TYPE_AHCI || type > BLKDEV_TYPE_IDE ||
        !sector_size || (sector_size & (sector_size - 1)) || sector_count > UINT64_MAX / sector_size ||
        geometry_source < BLKDEV_GEOMETRY_UNKNOWN || geometry_source > BLKDEV_GEOMETRY_ATA ||
        (geometry_source == BLKDEV_GEOMETRY_SOFTWARE && type != BLKDEV_TYPE_RAMDISK) ||
        (geometry_source == BLKDEV_GEOMETRY_ATA && type != BLKDEV_TYPE_AHCI)) return -1;
    for (int i = 0; i < BLKDEV_MAX_DEVICES; i++) {
        if (!devices[i].in_use) {
            devices[i].in_use = 1;
            devices[i].type = type;
            devices[i].sector_count = sector_count;
            devices[i].sector_size = sector_size;
            devices[i].geometry_source = geometry_source;
            strncpy(devices[i].name, name, sizeof(devices[i].name) - 1);
            devices[i].name[sizeof(devices[i].name) - 1] = '\0';
            memset(&devices[i].ops, 0, sizeof(devices[i].ops));
            if (ops) devices[i].ops = *ops;
            devices[i].driver_data = driver_data;
            return i;
        }
    }
    return -1;
}

void blkdev_unregister(uint8_t dev_id)
{
    if (sealed) return;
    if (dev_id < BLKDEV_MAX_DEVICES) {
        devices[dev_id].in_use = 0;
        devices[dev_id].type = BLKDEV_TYPE_NONE;
    }
}

int blkdev_seal(void) { if (sealed) return -16; sealed = 1; return 0; }
int blkdev_is_sealed(void) { return sealed; }

static int range_valid(const blkdev_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    return lba <= d->sector_count && count <= d->sector_count - lba && (!count || buf);
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
    if (!range_valid(&devices[dev_id], lba, count, buf)) return -22;
    if (!count) return 0;
    if (!devices[dev_id].ops.read)
        return -2;
    return devices[dev_id].ops.read(dev_id, lba, count, buf);
}

int blkdev_write(uint8_t dev_id, uint64_t lba, uint32_t count, const void *buf)
{
    if (dev_id >= BLKDEV_MAX_DEVICES || !devices[dev_id].in_use)
        return -1;
    if (!range_valid(&devices[dev_id], lba, count, buf)) return -22;
    if (!count) return 0;
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
