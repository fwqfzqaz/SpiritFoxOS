#include "block.h"

int sfm_block_describe(const blkdev_t *d, sfm_block_record_t *out)
{
    if (!d || !out || !d->in_use || d->id >= SFM_BLOCK_MAX ||
        d->type < BLKDEV_TYPE_AHCI || d->type > BLKDEV_TYPE_IDE || !d->sector_size ||
        (d->sector_size & (d->sector_size - 1)) || d->sector_count > UINT64_MAX / d->sector_size)
        return SFM_EINVAL;
    unsigned length = 0;
    while (length < sizeof(d->name) && d->name[length]) length++;
    if (!length || length == sizeof(d->name)) return SFM_EINVAL;
    sfm_clear(out, sizeof(*out));
    out->version = SFM_BLOCK_VERSION; out->size = sizeof(*out);
    out->sector_count = d->sector_count; out->sector_size = d->sector_size;
    out->index = d->id;
    switch (d->type) {
    case BLKDEV_TYPE_AHCI: out->type = SFM_BLOCK_AHCI; break;
    case BLKDEV_TYPE_RAMDISK: out->type = SFM_BLOCK_RAMDISK; break;
    case BLKDEV_TYPE_IDE: out->type = SFM_BLOCK_IDE; break;
    default: return SFM_EINVAL;
    }
    if (d->type != BLKDEV_TYPE_RAMDISK) out->flags = SFM_BLOCK_GEOMETRY_UNVERIFIED;
    sfm_copy(out->name, d->name, length);
    return 0;
}

int sfm_blocks_publish(void)
{
    uint64_t endpoint = sfm_lookup("dev.block");
    if (!endpoint) return SFM_ENOENT;
    sfm_message_t *req = sf_alloc(sizeof(*req)), *resp = sf_alloc(sizeof(*resp));
    int result = SFM_ENOMEM;
    if (!req || !resp) goto done;
    uint32_t count = 0;
    for (uint32_t i = 0; i < SFM_BLOCK_MAX; i++) {
        sfm_block_record_t item;
        result = sf_block_read(i, &item);
        if (result < 0) goto done;
        if (!result) continue;
        sfm_clear(req, sizeof(*req)); req->type = SFM_BLOCK_REGISTER; req->flags = SFM_BLOCK_VERSION;
        req->size = sizeof(item); sfm_copy(req->data, &item, sizeof(item));
        result = sfm_call(endpoint, req, resp);
        if (result) goto done;
        count++;
    }
    sfm_clear(req, sizeof(*req)); req->type = SFM_BLOCK_SEAL; req->flags = SFM_BLOCK_VERSION;
    result = sfm_call(endpoint, req, resp);
    if (result) goto done;
    uint32_t ordinal = 0;
    for (uint32_t i = 0; i < SFM_BLOCK_MAX; i++) {
        sfm_block_record_t expected;
        result = sf_block_read(i, &expected);
        if (result < 0) goto done;
        if (!result) continue;
        expected.snapshot = endpoint;
        req->type = SFM_BLOCK_ENUM; req->handle = 0; req->offset = ordinal;
        result = sfm_call(endpoint, req, resp);
        if (result) goto done;
        if (resp->result != 1 || resp->size != sizeof(expected) || resp->handle != endpoint ||
            resp->offset != ordinal) { result = SFM_EINVAL; goto done; }
        for (size_t j = 0; j < sizeof(expected); j++)
            if (resp->data[j] != ((uint8_t *)&expected)[j]) { result = SFM_EINVAL; goto done; }
        req->type = SFM_BLOCK_GET; req->handle = endpoint; req->offset = i;
        result = sfm_call(endpoint, req, resp);
        if (result || resp->result != 1 || resp->size != sizeof(expected) ||
            resp->handle != endpoint || resp->offset != ordinal) { result = SFM_EINVAL; goto done; }
        for (size_t j = 0; j < sizeof(expected); j++)
            if (resp->data[j] != ((uint8_t *)&expected)[j]) { result = SFM_EINVAL; goto done; }
        ordinal++;
    }
    req->type = SFM_BLOCK_ENUM; req->handle = 0; req->offset = count;
    result = sfm_call(endpoint, req, resp);
    if (!result && (ordinal != count || resp->result || resp->size)) result = SFM_EINVAL;
    if (!result) sf_log("[sfmod] sealed block inventory matches platform devices\n");
done:
    sf_free(req); sf_free(resp);
    return result;
}
