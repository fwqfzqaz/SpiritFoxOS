#include "internal.h"
#include "block_client.h"

/* Prevent same-CPU preemption while a synchronous handler owns its endpoint.
 * Other CPUs/recursive callers may return EBUSY. Never spin or sleep here. */
static int call(uint64_t endpoint, sfm_message_t *req, sfm_message_t *resp)
{
    sfm_clear(resp, sizeof(*resp));
    uint64_t irq = sf_irq_save();
    int result = sfm_call(endpoint, req, resp);
    sf_irq_restore(irq);
    if (result) return result < 0 ? result : -5; /* Refusal need not write resp. */
    if (resp->src != endpoint || resp->dst != req->src || resp->type != req->type ||
        resp->sequence != req->sequence || resp->route) return -5;
    return 0;
}

int sf_block_client_open(uint32_t index, sf_block_client_t *out)
{
    if (!out || index >= SFM_BLOCK_MAX) return SFM_EINVAL;
    uint64_t endpoint = sfm_lookup("dev.block");
    if (!endpoint) return SFM_ENOENT;
    sfm_message_t *req = sf_alloc(sizeof(*req)), *resp = sf_alloc(sizeof(*resp));
    int result = SFM_ENOMEM;
    if (!req || !resp) goto done;
    sfm_clear(req, sizeof(*req));
    req->type = SFM_BLOCK_GET; req->flags = SFM_BLOCK_VERSION;
    req->handle = endpoint; req->offset = index;
    result = call(endpoint, req, resp);
    if (result) goto done;
    result = -5;
    if (resp->result != 1 || resp->size != sizeof(sfm_block_record_t) ||
        resp->handle != endpoint || resp->offset >= SFM_BLOCK_MAX) goto done;
    sfm_block_record_t r;
    sfm_copy(&r, resp->data, sizeof(r));
    if (r.version != SFM_BLOCK_VERSION || r.size != sizeof(r) || r.snapshot != endpoint ||
        r.index != index || r.reserved || r.flags || !r.sector_size ||
        r.sector_size > SFM_PAYLOAD || (r.sector_size & (r.sector_size - 1)) ||
        r.sector_count > UINT64_MAX / r.sector_size ||
        !((r.geometry_source == SFM_BLOCK_GEOMETRY_SOFTWARE && r.type == SFM_BLOCK_RAMDISK) ||
          (r.geometry_source == SFM_BLOCK_GEOMETRY_ATA && r.type == SFM_BLOCK_AHCI))) goto done;
    *out = (sf_block_client_t){endpoint, r.sector_count, index, r.sector_size};
    result = 0;
done:
    sf_free(req); sf_free(resp);
    return result;
}

int sf_block_client_read(const sf_block_client_t *d, uint64_t lba, uint32_t count,
                         void *data, uint32_t capacity)
{
    if (!d || !d->endpoint || d->index >= SFM_BLOCK_MAX || !data || !count ||
        !d->sector_size || d->sector_size > SFM_PAYLOAD ||
        (d->sector_size & (d->sector_size - 1)) ||
        count > SF_BLOCK_CLIENT_MAX_BYTES / d->sector_size ||
        count > capacity / d->sector_size) return SFM_EINVAL;
    uint32_t bytes = count * d->sector_size;
    sfm_clear(data, bytes);
    if (lba > d->sector_count || count > d->sector_count - lba) return SFM_EINVAL;
    sfm_message_t *req = sf_alloc(sizeof(*req)), *resp = sf_alloc(sizeof(*resp));
    int result = SFM_ENOMEM;
    if (!req || !resp) goto done;
    sfm_clear(req, sizeof(*req));
    req->type = SFM_BLOCK_READ; req->flags = SFM_BLOCK_VERSION;
    req->handle = d->endpoint; req->size = sizeof(sfm_block_read_t);
    uint32_t done_sectors = 0;
    while (done_sectors < count) {
        uint32_t part = count - done_sectors;
        if (part > SFM_PAYLOAD / d->sector_size) part = SFM_PAYLOAD / d->sector_size;
        sfm_block_read_t read = {.index = d->index, .count = part};
        req->offset = lba + done_sectors; req->sequence = done_sectors + 1;
        sfm_copy(req->data, &read, sizeof(read));
        result = call(d->endpoint, req, resp);
        if (result) goto done;
        uint32_t length = part * d->sector_size;
        if (resp->handle != d->endpoint || resp->offset != req->offset ||
            resp->size != length || resp->result != length) { result = -5; goto done; }
        sfm_copy((uint8_t *)data + done_sectors * d->sector_size, resp->data, length);
        done_sectors += part;
    }
done:
    if (result) sfm_clear(data, bytes);
    sf_free(req); sf_free(resp);
    return result;
}
