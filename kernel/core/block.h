#ifndef SFM_BLOCK_INTERNAL_H
#define SFM_BLOCK_INTERNAL_H
#include "internal.h"
#include "../include/blkdev.h"
int sfm_block_describe(const blkdev_t *, sfm_block_record_t *);
/* Read one legacy slot: 1 present, 0 unused, negative invalid. */
int sf_block_read(uint32_t, sfm_block_record_t *);
int sfm_blocks_publish(void);
int sf_block_freeze(void);
int sf_block_frozen(void);
void *sf_block_dma_page(void);
int sf_block_raw_read(uint32_t, uint64_t, uint32_t, void *);
int sfm_block_io_selftest(void);
#endif
