#include "block.h"
#include "memory.h"
_Static_assert(BLKDEV_MAX_DEVICES == SFM_BLOCK_MAX, "block inventory capacity");
int sf_block_read(uint32_t index, sfm_block_record_t *out)
{
    if (!out || index >= SFM_BLOCK_MAX) return SFM_EINVAL;
    const blkdev_t *device = blkdev_get((uint8_t)index);
    if (!device) return 0;
    int result = sfm_block_describe(device, out);
    return result ? result : 1;
}
int sf_block_freeze(void) { return blkdev_seal(); }
int sf_block_frozen(void) { return blkdev_is_sealed(); }
void *sf_block_dma_page(void) { return alloc_page(); }
int sf_block_raw_read(uint32_t index, uint64_t lba, uint32_t count, void *buffer)
{
    if (index >= SFM_BLOCK_MAX) return SFM_EINVAL;
    return blkdev_read((uint8_t)index, lba, count, buffer);
}
int sf_block_raw_write(uint32_t index, uint64_t lba, uint32_t count, const void *buffer)
{
    if (index >= SFM_BLOCK_MAX) return SFM_EINVAL;
    return blkdev_write((uint8_t)index, lba, count, buffer);
}
