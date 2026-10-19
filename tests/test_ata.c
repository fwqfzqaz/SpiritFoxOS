#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/include/ata_geometry.h"
#include "../kernel/include/ahci.h"

static void identify(uint16_t w[256], uint64_t count)
{
    memset(w, 0, 512); w[49] = 0x300; w[83] = 0x4400; w[106] = 0x4000;
    for (unsigned i = 0; i < 4; i++) w[100 + i] = (uint16_t)(count >> (16 * i));
}
int main(void)
{
    uint16_t w[256]; uint64_t capacity = 0;
    identify(w, 0x123456789ab);
    assert(!ata_identify_geometry(w, &capacity) && capacity == 0x123456789ab);
    w[106] = 0x6003; /* 512-byte logical sectors, eight per physical sector. */
    assert(!ata_identify_geometry(w, &capacity));
    for (unsigned bad = 0; bad < 12; bad++) {
        identify(w, 131072); capacity = 0x12345;
        switch (bad) {
        case 0: w[0] = 0x8000; break;
        case 1: w[49] = 0x200; break;
        case 2: w[49] = 0x100; break;
        case 3: w[83] = 0x400; break;
        case 4: w[83] = 0xc400; break;
        case 5: w[83] = 0x4000; break;
        case 6: w[106] = 0x5000; w[117] = 2048; break; /* 4Kn unsupported. */
        case 7: w[106] = 0x5000; w[117] = 256; break; /* Inconsistent size field. */
        case 8: identify(w, 0); break;
        case 9: identify(w, (1ull << 48) + 1); break;
        case 10: memset(w, 0, sizeof(w)); break;
        case 11: memset(w, 0xff, sizeof(w)); break;
        }
        assert(ata_identify_geometry(w, &capacity) < 0 && capacity == 0x12345);
    }
    identify(w, 1ull << 48); assert(!ata_identify_geometry(w, &capacity) && capacity == (1ull << 48));
    assert(ata_identify_geometry(NULL, &capacity) == -22 && ata_identify_geometry(w, NULL) == -22);
    uint8_t fis[20];
    assert(!ata_command_fis(fis, ATA_READ_DMA_EXT, 0x123456789abc, 0x2345));
    uint8_t expected[20] = {0x27, 0x80, 0x25, 0, 0xbc, 0x9a, 0x78, 0x40, 0x56, 0x34, 0x12, 0, 0x45, 0x23};
    assert(!memcmp(fis, expected, sizeof(fis)));
    assert(!ata_command_fis(fis, ATA_WRITE_DMA_EXT, (1ull << 48) - 1, 1) && fis[2] == 0x35);
    assert(!ata_command_fis(fis, ATA_READ_DMA_EXT, 0, 65535) && fis[12] == 255 && fis[13] == 255);
    assert(ata_command_fis(fis, ATA_READ_DMA_EXT, 0, 0) == -22);
    assert(ata_command_fis(fis, ATA_READ_DMA_EXT, 0, 65536) == -22);
    assert(ata_command_fis(fis, ATA_READ_DMA_EXT, 1ull << 48, 1) == -22);
    assert(ata_command_fis(fis, ATA_READ_DMA_EXT, (1ull << 48) - 1, 2) == -22);
    assert(!ata_command_fis(fis, ATA_IDENTIFY, 0, 1));
    assert(ata_command_fis(fis, ATA_IDENTIFY, 1, 1) == -22);
    assert(ata_command_fis(fis, ATA_IDENTIFY, 0, 2) == -22);
    assert(ata_command_fis(fis, 0xff, 0, 1) == -38);
    puts("ATA tests passed: verified geometry, unsupported sectors, LBA48 FIS byte vectors and boundaries");
    return 0;
}
