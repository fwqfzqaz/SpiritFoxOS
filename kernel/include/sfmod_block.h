/* Frozen boot inventory only; no data I/O or device access grants. */
#ifndef SFM_BLOCK_H
#define SFM_BLOCK_H
#include <stdint.h>
#include <stddef.h>
#define SFM_BLOCK_VERSION 1u
#define SFM_BLOCK_MAX 16u
#define SFM_BLOCK_REGISTER 0x5000u
#define SFM_BLOCK_SEAL 0x5001u
#define SFM_BLOCK_ENUM 0x5002u
#define SFM_BLOCK_GET 0x5003u
#define SFM_BLOCK_AHCI 1u
#define SFM_BLOCK_RAMDISK 2u
#define SFM_BLOCK_IDE 3u
/* The legacy driver may substitute geometry after failed IDENTIFY. */
#define SFM_BLOCK_GEOMETRY_UNVERIFIED 1u
typedef struct sfm_block_record {
    uint32_t version, size;
    uint64_t snapshot, sector_count;
    uint32_t sector_size, flags, index, type;
    char name[16];
    uint32_t reserved[2];
} sfm_block_record_t;
_Static_assert(sizeof(sfm_block_record_t) == 64, "block record ABI");
_Static_assert(offsetof(sfm_block_record_t, name) == 40, "block name ABI");
#endif
