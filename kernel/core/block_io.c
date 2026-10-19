/* Pages remain core-owned for the boot lifetime. An I/O error quarantines the
 * device page: a timed-out legacy controller may still DMA into that memory. */
#include "block.h"
#include "block_client.h"
static void *pages[SFM_BLOCK_MAX], *write_pages[SFM_BLOCK_MAX];
static unsigned char failed[SFM_BLOCK_MAX], busy[SFM_BLOCK_MAX];

int sf_block_read_sectors(uint32_t index, uint64_t lba, uint32_t count, void *data, uint32_t capacity)
{
    if (!data || index >= SFM_BLOCK_MAX || !count || !capacity || capacity > SFM_PAYLOAD) return SFM_EINVAL;
    if (!sf_block_frozen()) return SFM_EAGAIN;
    uint64_t irq = sf_irq_save();
    if (__atomic_test_and_set(&busy[index], __ATOMIC_ACQUIRE)) {
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
    __atomic_clear(&busy[index], __ATOMIC_RELEASE);
    sf_irq_restore(irq);
    return result;
}

int sf_block_client_write_compat(const sf_block_client_t *d, uint64_t lba, uint32_t count,
                                const void *data, uint32_t capacity, uint32_t *completed)
{
    if (completed) *completed = 0;
    if (!d || !completed || !data || d->index >= SFM_BLOCK_MAX || !count ||
        !d->sector_size || d->sector_size > SFM_PAYLOAD ||
        count > SF_BLOCK_CLIENT_MAX_BYTES / d->sector_size ||
        count > capacity / d->sector_size) return SFM_EINVAL;
    if (!sf_block_frozen()) return SFM_EAGAIN;
    uint32_t index = d->index;
    uint64_t irq = sf_irq_save();
    if (__atomic_test_and_set(&busy[index], __ATOMIC_ACQUIRE)) {
        sf_irq_restore(irq); return SFM_EBUSY;
    }
    int result = SFM_ENOENT;
    if (!d->endpoint || sfm_lookup("dev.block") != d->endpoint) goto done;
    sfm_block_record_t device;
    result = sf_block_read(index, &device);
    if (result <= 0) { if (!result) result = SFM_ENOENT; goto done; }
    result = SFM_EINVAL;
    if (device.sector_size != d->sector_size || device.sector_count != d->sector_count ||
        (device.sector_size & (device.sector_size - 1)) ||
        lba > device.sector_count || count > device.sector_count - lba) goto done;
    if (device.geometry_source == SFM_BLOCK_GEOMETRY_UNKNOWN || device.flags) { result = -19; goto done; }
    if (!((device.geometry_source == SFM_BLOCK_GEOMETRY_SOFTWARE && device.type == SFM_BLOCK_RAMDISK) ||
          (device.geometry_source == SFM_BLOCK_GEOMETRY_ATA && device.type == SFM_BLOCK_AHCI))) goto done;
    if (failed[index]) { result = -5; goto done; }
    if (!write_pages[index]) write_pages[index] = sf_block_dma_page();
    if (!write_pages[index]) { result = SFM_ENOMEM; goto done; }
    while (*completed < count) {
        uint32_t part = count - *completed;
        if (part > SFM_PAYLOAD / d->sector_size) part = SFM_PAYLOAD / d->sector_size;
        sfm_clear(write_pages[index], SFM_PAYLOAD);
        sfm_copy(write_pages[index], (const uint8_t *)data + *completed * d->sector_size,
                 part * d->sector_size);
        result = sf_block_raw_write(index, lba + *completed, part, write_pages[index]);
        if (result) {
            /* EBUSY guarantees no command was issued. All other backend errors
             * are ambiguous: freeze BOTH directions and never reuse either page. */
            if (result != SFM_EBUSY) { failed[index] = 1; result = -5; }
            goto done;
        }
        *completed += part;
    }
done:
    __atomic_clear(&busy[index], __ATOMIC_RELEASE);
    sf_irq_restore(irq);
    return result;
}
