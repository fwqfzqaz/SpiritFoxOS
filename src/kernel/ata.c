#include "ata.h"
#include "log.h"
#include "../include/io.h"
#include "../include/string.h"

static ata_device_t ata_devices[ATA_MAX_DEVICES];
static int ata_device_count = 0;

static void ata_io_wait(uint16_t io_base) {
    inb(io_base + 0x0C);
    inb(io_base + 0x0C);
    inb(io_base + 0x0C);
    inb(io_base + 0x0C);
}

static int ata_wait_bsy(uint16_t io_base) {
    uint32_t timeout = 1000000;
    while (timeout--) {
        uint8_t status = inb(io_base + 0x07);
        if (!(status & ATA_STATUS_BSY)) return 0;
    }
    return -1;
}

static int ata_wait_drq(uint16_t io_base) {
    uint32_t timeout = 1000000;
    while (timeout--) {
        uint8_t status = inb(io_base + 0x07);
        if (status & ATA_STATUS_ERR) return -1;
        if (status & ATA_STATUS_DF) return -1;
        if (status & ATA_STATUS_DRQ) return 0;
        if (!(status & ATA_STATUS_BSY)) return -1;
    }
    return -1;
}

static void ata_select_drive(uint16_t io_base, uint8_t drive) {
    outb(io_base + 0x06, drive);
    ata_io_wait(io_base);
}

static int ata_identify(uint16_t io_base, uint8_t drive, uint16_t *buf) {
    ata_select_drive(io_base, drive);
    ata_io_wait(io_base);

    outb(io_base + 0x02, 0);
    outb(io_base + 0x03, 0);
    outb(io_base + 0x04, 0);
    outb(io_base + 0x05, 0);

    outb(io_base + 0x07, ATA_CMD_IDENTIFY);

    uint8_t status = inb(io_base + 0x07);
    if (status == 0) return -1;

    if (ata_wait_bsy(io_base) != 0) return -1;

    status = inb(io_base + 0x07);
    if (status & ATA_STATUS_ERR) return -1;

    uint8_t cl = inb(io_base + 0x04);
    uint8_t ch = inb(io_base + 0x05);
    if (cl == 0x14 && ch == 0xEB) return -1;
    if (cl == 0x00 && ch == 0x00) return -1;

    if (ata_wait_drq(io_base) != 0) return -1;

    for (int i = 0; i < 256; i++) {
        buf[i] = inw(io_base + 0x00);
    }

    return 0;
}

static void ata_swap_string(uint16_t *src, char *dst, int words) {
    for (int i = 0; i < words; i++) {
        dst[i * 2] = (char)(src[i] >> 8);
        dst[i * 2 + 1] = (char)(src[i] & 0xFF);
    }
    dst[words * 2] = '\0';
    int len = words * 2;
    while (len > 0 && dst[len - 1] == ' ') dst[--len] = '\0';
}

static void ata_detect_bus(uint16_t io_base, uint16_t ctrl_base) {
    outb(ctrl_base, 0x02);
    ata_io_wait(io_base);

    uint8_t drives[] = {ATA_MASTER, ATA_SLAVE};

    for (int d = 0; d < 2; d++) {
        if (ata_device_count >= ATA_MAX_DEVICES) return;

        uint16_t id_buf[256];
        if (ata_identify(io_base, drives[d], id_buf) != 0) continue;

        ata_device_t *dev = &ata_devices[ata_device_count];
        dev->io_base = io_base;
        dev->ctrl_base = ctrl_base;
        dev->drive = drives[d];
        dev->present = 1;

        dev->lba48 = (id_buf[83] & 0x04) ? 1 : 0;

        if (dev->lba48) {
            dev->sectors_48 = ((uint64_t)id_buf[103] << 48) |
                              ((uint64_t)id_buf[102] << 32) |
                              ((uint64_t)id_buf[101] << 16) |
                              ((uint64_t)id_buf[100]);
        }

        dev->sectors_28 = ((uint32_t)id_buf[61] << 16) | id_buf[60];

        ata_swap_string(&id_buf[27], dev->model, 20);

        uint64_t total_sectors = dev->lba48 ? dev->sectors_48 : dev->sectors_28;
        uint64_t size_mb = (total_sectors * 512) / (1024 * 1024);

        LOG_I("ata", "Detected %s at %04x:%s - %s (%u MB, %s)",
              d == 0 ? "Master" : "Slave",
              io_base,
              dev->lba48 ? "LBA48" : "LBA28",
              dev->model,
              (uint32_t)size_mb,
              dev->lba48 ? "LBA48" : "LBA28");

        ata_device_count++;
    }
}

void ata_init(void) {
    ata_device_count = 0;
    memset(ata_devices, 0, sizeof(ata_devices));

    ata_detect_bus(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL);
    ata_detect_bus(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL);

    LOG_I("ata", "Found %d ATA device(s)", ata_device_count);
}

int ata_read_sectors(ata_device_t *dev, uint64_t lba, uint32_t count, void *buf) {
    if (!dev || !dev->present || count == 0) return -1;

    uint16_t io = dev->io_base;

    ata_select_drive(io, dev->drive | ((lba >> 24) & 0x0F));
    ata_io_wait(io);

    if (ata_wait_bsy(io) != 0) return -1;

    outb(io + 0x01, 0x00);
    outb(io + 0x02, (uint8_t)count);
    outb(io + 0x03, (uint8_t)(lba & 0xFF));
    outb(io + 0x04, (uint8_t)((lba >> 8) & 0xFF));
    outb(io + 0x05, (uint8_t)((lba >> 16) & 0xFF));
    outb(io + 0x06, dev->drive | ((lba >> 24) & 0x0F));
    outb(io + 0x07, ATA_CMD_READ_SECTORS);

    uint16_t *dst = (uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (ata_wait_bsy(io) != 0) return -1;
        if (ata_wait_drq(io) != 0) return -1;

        for (int i = 0; i < 256; i++) {
            dst[s * 256 + i] = inw(io + 0x00);
        }
    }

    return 0;
}

int ata_write_sectors(ata_device_t *dev, uint64_t lba, uint32_t count, const void *buf) {
    if (!dev || !dev->present || count == 0) return -1;

    uint16_t io = dev->io_base;

    ata_select_drive(io, dev->drive | ((lba >> 24) & 0x0F));
    ata_io_wait(io);

    if (ata_wait_bsy(io) != 0) return -1;

    outb(io + 0x01, 0x00);
    outb(io + 0x02, (uint8_t)count);
    outb(io + 0x03, (uint8_t)(lba & 0xFF));
    outb(io + 0x04, (uint8_t)((lba >> 8) & 0xFF));
    outb(io + 0x05, (uint8_t)((lba >> 16) & 0xFF));
    outb(io + 0x06, dev->drive | ((lba >> 24) & 0x0F));
    outb(io + 0x07, ATA_CMD_WRITE_SECTORS);

    const uint16_t *src = (const uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (ata_wait_bsy(io) != 0) return -1;
        if (ata_wait_drq(io) != 0) return -1;

        for (int i = 0; i < 256; i++) {
            outw(io + 0x00, src[s * 256 + i]);
        }

        ata_io_wait(io);
    }

    outb(io + 0x07, ATA_CMD_FLUSH_CACHE);
    ata_wait_bsy(io);

    return 0;
}

void ata_flush_cache(ata_device_t *dev) {
    if (!dev || !dev->present) return;
    outb(dev->io_base + 0x07, ATA_CMD_FLUSH_CACHE);
    ata_wait_bsy(dev->io_base);
}

ata_device_t *ata_get_device(int index) {
    if (index < 0 || index >= ata_device_count) return NULL;
    return &ata_devices[index];
}

int ata_get_device_count(void) {
    return ata_device_count;
}
