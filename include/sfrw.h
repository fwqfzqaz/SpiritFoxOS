#ifndef SF_RW_H
#define SF_RW_H
#include "sf.h"
/* SFRWFS1: bounded writable filesystem format core (v1).
 *
 * All offsets are little-endian.  The device vtable works in 4096-byte
 * blocks.  Layout:
 *   0        primary superblock (4064 bytes + sha256)
 *   1        backup superblock (identical)
 *   2..33    inode table (32 blocks x 32 inodes x 128 bytes)
 *   34..35   allocation bitmap (2 blocks = 65536 bits)
 *   36..99   journal (header in 36, up to 16 payload blocks from 37)
 *   100..    data blocks (directories, file data, indirect blocks)
 */
#define SF_RW_BLOCK_BYTES 4096u
#define SF_RW_MAX_BLOCKS 65536u
#define SF_RW_MIN_BLOCKS 128u
#define SF_RW_MAX_INODES 1024u
#define SF_RW_INODE_BYTES 128u
#define SF_RW_INODES_PER_BLOCK 32u
#define SF_RW_INODE_TABLE_BLOCK 2u
#define SF_RW_INODE_TABLE_BLOCKS 32u
#define SF_RW_BITMAP_BLOCK 34u
#define SF_RW_BITMAP_BLOCKS 2u
#define SF_RW_BITMAP_BYTES (SF_RW_BLOCK_BYTES*SF_RW_BITMAP_BLOCKS)
#define SF_RW_JOURNAL_BLOCK 36u
#define SF_RW_JOURNAL_BLOCKS 64u
#define SF_RW_DATA_BLOCK 100u
#define SF_RW_JOURNAL_MAX 16u
#define SF_RW_NAME_MAX 95u
#define SF_RW_PATH_MAX 255u
#define SF_RW_DIRECT_BLOCKS 12u
#define SF_RW_INDIRECT_ENTRIES 1024u
#define SF_RW_FILE_BLOCKS (SF_RW_DIRECT_BLOCKS+SF_RW_INDIRECT_ENTRIES)
#define SF_RW_FILE_MAX (SF_RW_FILE_BLOCKS*SF_RW_BLOCK_BYTES)
#define SF_RW_ROOT 1u
#define SF_RW_QUOTA_MAX 8u
#define SF_RW_UID_TRACK 16u

/* inode mode: type bits plus low 12 permission bits. */
#define SF_RW_TYPE_DIR 0x4000u
#define SF_RW_TYPE_FILE 0x8000u
#define SF_RW_TYPE_MASK 0xF000u
#define SF_RW_PERM_MASK 0777u
/* Directory entry types. */
#define SF_RW_ENTRY_FILE 1u
#define SF_RW_ENTRY_DIR 2u
/* open() flags. */
#define SF_RW_READ 1u
#define SF_RW_WRITE 2u

typedef int (*SfrwRead)(void*,uint32_t,uint8_t*);
typedef int (*SfrwWrite)(void*,uint32_t,const uint8_t*);
typedef int (*SfrwFlush)(void*);
typedef struct { SfrwRead read; SfrwWrite write; SfrwFlush flush; void* context; } SfrwDev;

typedef struct { uint32_t uid, blocks, inodes, reserved; } SfrwQuota;
typedef struct { uint32_t block_count, default_quota_blocks, default_quota_inodes; } SfrwFormatOptions;
typedef struct { uint32_t inode, mode, uid, gid, links, blocks; uint64_t size; } SfrwStat;
_Static_assert(sizeof(SfrwStat)==32,"writable fs stat ABI");
typedef struct {
 uint32_t checked_inodes, used_inodes, used_blocks, free_blocks, free_inodes;
 uint32_t lost_inodes, unreachable_blocks, duplicate_blocks, bad_entries, fixed_links, repaired;
} SfckReport;

typedef struct { uint32_t uid, blocks, inodes; } SfrwUsage;

typedef struct {
 SfrwDev dev;
 uint32_t block_count, free_blocks, free_inodes;
 uint64_t generation;
 uint32_t default_quota_blocks, default_quota_inodes, quota_count;
 SfrwQuota quota[SF_RW_QUOTA_MAX];
 uint8_t bitmap[SF_RW_BITMAP_BYTES];
 uint8_t block[SF_RW_BLOCK_BYTES];
 uint8_t aux[SF_RW_BLOCK_BYTES];
 uint8_t indirect[SF_RW_BLOCK_BYTES];
 uint32_t journal_target[SF_RW_JOURNAL_MAX];
 uint32_t journal_length[SF_RW_JOURNAL_MAX];
 uint8_t journal_hash[SF_RW_JOURNAL_MAX][32];
 uint32_t journal_count, journal_clean;
 uint32_t usage_count;
 SfrwUsage usage[SF_RW_UID_TRACK];
 /* COW write staging: existing and replacement block per touched logical block. */
 uint32_t old_block[SF_RW_FILE_BLOCKS];
 uint32_t new_block[SF_RW_FILE_BLOCKS];
 /* fsck scratch. */
 uint8_t ref[SF_RW_BITMAP_BYTES];
 uint8_t reach[SF_RW_MAX_INODES/8];
 uint16_t refs[SF_RW_MAX_INODES];
 uint16_t subdirs[SF_RW_MAX_INODES];
 uint32_t queue[SF_RW_MAX_INODES];
 uint32_t mounted, dirty;
} Sfrw;

int sf_rw_format(Sfrw*,SfrwDev,const SfrwFormatOptions*);
int sf_rw_mount(Sfrw*,SfrwDev);
int sf_rw_fsck(Sfrw*,SfrwDev,SfckReport*);
int sf_rw_open(Sfrw*,const char*,uint32_t,uint32_t,uint32_t*);
int sf_rw_create(Sfrw*,const char*,uint32_t,uint16_t);
int sf_rw_mkdir(Sfrw*,const char*,uint32_t,uint16_t);
int sf_rw_stat(Sfrw*,uint32_t,SfrwStat*);
int sf_rw_readdir(Sfrw*,uint32_t,uint32_t,uint32_t,char[SF_RW_NAME_MAX+1],uint32_t*,uint8_t*);
long sf_rw_read(Sfrw*,uint32_t,uint32_t,uint64_t,void*,size_t);
long sf_rw_write(Sfrw*,uint32_t,uint32_t,uint64_t,const void*,size_t);
int sf_rw_truncate(Sfrw*,uint32_t,uint32_t,uint64_t);
int sf_rw_unlink(Sfrw*,const char*,uint32_t);
int sf_rw_rmdir(Sfrw*,const char*,uint32_t);
int sf_rw_rename(Sfrw*,const char*,const char*,uint32_t);
int sf_rw_fsync(Sfrw*);
int sf_rw_set_quota(Sfrw*,uint32_t,uint32_t,uint32_t,uint32_t);
#endif
