/* Registration is serialized trusted boot work, not an authorization boundary. */
#include "sfmod.h"
static const sfm_api_t *core;
static uint32_t owner_id, count;
static uint64_t endpoint;
static int sealed;
static sfm_block_record_t devices[SFM_BLOCK_MAX];

static int valid(const sfm_block_record_t *r)
{
    if (r->version != SFM_BLOCK_VERSION || r->size != sizeof(*r) || r->snapshot ||
        r->index >= SFM_BLOCK_MAX || r->type < SFM_BLOCK_AHCI || r->type > SFM_BLOCK_IDE ||
        !r->sector_size || (r->sector_size & (r->sector_size - 1)) ||
        r->sector_count > UINT64_MAX / r->sector_size ||
        (r->flags & ~SFM_BLOCK_GEOMETRY_UNVERIFIED) || r->reserved[0] || r->reserved[1]) return 0;
    unsigned length = 0;
    while (length < sizeof(r->name) && r->name[length]) {
        unsigned char c = (unsigned char)r->name[length++];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    if (!length || length == sizeof(r->name)) return 0;
    for (; length < sizeof(r->name); length++) if (r->name[length]) return 0;
    return 1;
}
static int receive(const sfm_message_t *req, sfm_message_t *resp)
{
    if (req->flags != SFM_BLOCK_VERSION) return -93;
    if (req->route) return -22;
    if (req->type == SFM_BLOCK_REGISTER) {
        if (sealed) return -30;
        if (req->handle || req->offset || req->size != sizeof(sfm_block_record_t)) return -22;
        sfm_block_record_t item;
        sfm_copy(&item, req->data, sizeof(item));
        if (!valid(&item)) return -22;
        for (uint32_t i = 0; i < count; i++)
            if (devices[i].index == item.index || sfm_name_equal(devices[i].name, item.name)) return -17;
        if (count == SFM_BLOCK_MAX) return -28;
        /* Keep ENUM ordered by legacy slot even if registration is sparse/out of order. */
        uint32_t position = count;
        while (position && devices[position - 1].index > item.index) {
            devices[position] = devices[position - 1]; position--;
        }
        item.snapshot = endpoint; devices[position] = item; count++;
        return 0;
    }
    if (req->type == SFM_BLOCK_SEAL) {
        if (req->size || req->handle || req->offset) return -22;
        if (sealed) return -16;
        sealed = 1; return 0;
    }
    if (req->type != SFM_BLOCK_ENUM && req->type != SFM_BLOCK_GET) return -38;
    if (req->size) return -22;
    if (!sealed) return -11;
    uint32_t ordinal = count;
    if (req->type == SFM_BLOCK_ENUM) {
        if (req->handle || req->offset > count) return -22;
        if (req->offset == count) return 0;
        ordinal = (uint32_t)req->offset;
    } else {
        if (req->handle != endpoint) return -2;
        if (req->offset >= SFM_BLOCK_MAX) return -22;
        for (uint32_t i = 0; i < count; i++) if (devices[i].index == req->offset) { ordinal = i; break; }
        if (ordinal == count) return -2;
    }
    resp->result = 1; resp->size = sizeof(devices[ordinal]);
    resp->handle = endpoint; resp->offset = ordinal;
    sfm_copy(resp->data, &devices[ordinal], resp->size);
    return 0;
}
static int start(const sfm_api_t *api, uint32_t owner)
{
    if (api->abi != SFM_ABI || api->size < sizeof(*api)) return -93;
    core = api; owner_id = owner; count = 0; sealed = 0;
    endpoint = core->endpoint_create(owner, "dev.block", receive);
    return endpoint ? 0 : -12;
}
static void stop(void)
{
    if (endpoint) core->endpoint_destroy(owner_id, endpoint);
    endpoint = 0; count = 0; sealed = 0;
}
SFM_MODULE("dev.block", start, stop);
