/* Pages remain core-owned for the boot lifetime. An I/O error quarantines the
 * device page: a timed-out legacy controller may still DMA into that memory. */
#include "block.h"
static void *pages[SFM_BLOCK_MAX];
static unsigned char failed[SFM_BLOCK_MAX], busy;

int sf_block_read_sectors(uint32_t index, uint64_t lba, uint32_t count, void *data, uint32_t capacity)
{
    if (!data || index >= SFM_BLOCK_MAX || !count || !capacity || capacity > SFM_PAYLOAD) return SFM_EINVAL;
    if (!sf_block_frozen()) return SFM_EAGAIN;
    uint64_t irq = sf_irq_save();
    if (__atomic_test_and_set(&busy, __ATOMIC_ACQUIRE)) {
        sf_irq_restore(irq); return SFM_EBUSY;
    }
    sfm_block_record_t device;
    int result = sf_block_read(index, &device);
    if (result <= 0) { if (!result) result = SFM_ENOENT; goto done; }
    if (device.geometry_source == SFM_BLOCK_GEOMETRY_UNKNOWN ||
        (device.flags & SFM_BLOCK_GEOMETRY_UNVERIFIED)) { result = -19; goto done; }
    if (!device.sector_size || count > capacity / device.sector_size ||
        lba > device.sector_count || count > device.sector_count - lba) { result = SFM_EINVAL; goto done; }
    if (failed[index]) { result = -5; goto done; }
    if (!pages[index]) pages[index] = sf_block_dma_page();
    if (!pages[index]) { result = SFM_ENOMEM; goto done; }
    uint32_t bytes = count * device.sector_size;
    sfm_clear(pages[index], SFM_PAYLOAD);
    result = sf_block_raw_read(index, lba, count, pages[index]);
    if (result) {
        if (result != SFM_EBUSY) { failed[index] = 1; result = -5; }
        goto done;
    }
    sfm_copy(data, pages[index], bytes);
done:
    __atomic_clear(&busy, __ATOMIC_RELEASE);
    sf_irq_restore(irq);
    return result;
}
