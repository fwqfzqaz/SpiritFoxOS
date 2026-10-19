#include "block.h"
_Static_assert(BLKDEV_MAX_DEVICES == SFM_BLOCK_MAX, "block inventory capacity");
int sf_block_read(uint32_t index, sfm_block_record_t *out)
{
    if (!out || index >= SFM_BLOCK_MAX) return SFM_EINVAL;
    const blkdev_t *device = blkdev_get((uint8_t)index);
    if (!device) return 0;
    int result = sfm_block_describe(device, out);
    return result ? result : 1;
}
