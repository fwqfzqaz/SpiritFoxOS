#ifndef ATA_H
#define ATA_H

#include <stdint.h>

#define ATA_PRIMARY_IO     0x1F0
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_IO   0x170
#define ATA_SECONDARY_CTRL 0x376

#define ATA_MASTER  0xA0
#define ATA_SLAVE   0xB0

#define ATA_SECTOR_SIZE 512

#define ATA_CMD_READ_SECTORS    0x20
#define ATA_CMD_WRITE_SECTORS   0x30
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_FLUSH_CACHE     0xE7

#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_DRDY 0x40
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01
#define ATA_STATUS_DF   0x20

typedef struct {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t  drive;
    uint8_t  present;
    uint8_t  lba48;
    uint32_t sectors_28;
    uint64_t sectors_48;
    char     model[41];
} ata_device_t;

#define ATA_MAX_DEVICES 4

void ata_init(void);
int ata_read_sectors(ata_device_t *dev, uint64_t lba, uint32_t count, void *buf);
int ata_write_sectors(ata_device_t *dev, uint64_t lba, uint32_t count, const void *buf);
ata_device_t *ata_get_device(int index);
int ata_get_device_count(void);
void ata_flush_cache(ata_device_t *dev);

#endif
