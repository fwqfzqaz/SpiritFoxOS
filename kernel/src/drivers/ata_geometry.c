#include "ata_geometry.h"
#include "ahci.h"

int ata_identify_geometry(const uint16_t w[256], uint64_t *sectors)
{
    if (!w || !sectors) return -22;
    /* Reject packet devices, missing DMA/LBA, invalid command-set words. */
    if ((w[0] & 0x8000) || (w[49] & 0x300) != 0x300 ||
        (w[83] & 0xc400) != 0x4400) return -19;
    /* A valid bit 12 advertises sectors larger than 512 bytes. Even an
     * inconsistent 117/118 field cannot authorize the fixed-512 transport. */
    if ((w[106] & 0xc000) == 0x4000 && (w[106] & 0x1000)) return -19;
    uint64_t value = (uint64_t)w[100] | ((uint64_t)w[101] << 16) |
                     ((uint64_t)w[102] << 32) | ((uint64_t)w[103] << 48);
    if (!value || value > (1ull << 48)) return -22;
    *sectors = value;
    return 0;
}

int ata_command_fis(uint8_t fis[20], uint8_t command, uint64_t lba, uint32_t count)
{
    if (!fis || !count || count > 65535) return -22;
    if (command == ATA_IDENTIFY) {
        if (lba || count != 1) return -22;
    } else if (command == ATA_READ_DMA_EXT || command == ATA_WRITE_DMA_EXT) {
        if (lba >= (1ull << 48) || count > (1ull << 48) - lba) return -22;
    } else return -38;
    for (unsigned i = 0; i < 20; i++) fis[i] = 0;
    fis[0] = AHCI_FIS_REG_H2D; fis[1] = 0x80; fis[2] = command;
    fis[7] = 0x40;
    for (unsigned i = 0; i < 3; i++) {
        fis[4 + i] = (uint8_t)(lba >> (8 * i));
        fis[8 + i] = (uint8_t)(lba >> (24 + 8 * i));
    }
    fis[12] = (uint8_t)count; fis[13] = (uint8_t)(count >> 8);
    return 0;
}
