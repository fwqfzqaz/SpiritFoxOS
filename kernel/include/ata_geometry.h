#ifndef ATA_GEOMETRY_H
#define ATA_GEOMETRY_H
#include <stdint.h>
/* The current transport supports DMA EXT and 512-byte logical sectors only. */
int ata_identify_geometry(const uint16_t words[256], uint64_t *sectors);
int ata_command_fis(uint8_t fis[20], uint8_t command, uint64_t lba, uint32_t count);
#endif
