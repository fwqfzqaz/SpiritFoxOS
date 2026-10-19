/* First device service: fixed /dev/null and /dev/zero policies live here.
 * Hardware devices and their namespace remain in the legacy devfs. */
#include "sfmod.h"

static const sfm_api_t *core;
static uint32_t owner_id;
static uint64_t endpoint;
static uint32_t references[3];
/* Distinct initial generation for diagnostics; route remains part of identity. */
#define DEVICE_GENERATION 0x80000001u

static uint64_t handle(uint32_t slot)
{ return ((uint64_t)DEVICE_GENERATION << 32) | (slot + 1u); }

static int resolve(uint64_t id)
{
    uint32_t slot = (uint32_t)id;
    if ((uint32_t)(id >> 32) != DEVICE_GENERATION || !slot || slot > 3) return -1;
    return (int)slot - 1;
}

static void describe(uint32_t slot, sfm_message_t *resp)
{
    resp->handle = handle(slot);
    resp->flags = slot ? SFM_NODE_CHARDEV : SFM_NODE_DIR;
    /* Linux-compatible major 1, minor 3/5. No memory address in the protocol. */
    resp->offset = slot ? 0x100u + (slot == 1 ? 3u : 5u) : 0;
}

static int named(const sfm_message_t *req, sfm_message_t *resp)
{
    if (req->handle != handle(0)) return -9;
    if (!req->size || req->size > SFM_FILE_NAME || req->data[req->size - 1]) return -22;
    for (uint32_t i = 0; i + 1 < req->size; i++) if (!req->data[i]) return -22;
    if (req->flags) return -30; /* Namespace changes require a later registration API. */
    uint32_t slot;
    if (req->size == 1) slot = 0;
    else if (req->size == sizeof("null") && sfm_name_equal((const char *)req->data, "null")) slot = 1;
    else if (req->size == sizeof("zero") && sfm_name_equal((const char *)req->data, "zero")) slot = 2;
    else return -2;
    describe(slot, resp);
    return 0;
}

static int receive(const sfm_message_t *req, sfm_message_t *resp)
{
    if (req->type == SFM_FS_ROOT)
    {
        if (req->size || req->offset) return -22;
        describe(0, resp);
        return 0;
    }
    if (req->type == SFM_FILE_OPEN) return named(req, resp);
    int slot = resolve(req->handle);
    if (slot < 0) return -9;
    switch (req->type)
    {
        case SFM_FILE_RETAIN:
            if (references[slot] == UINT32_MAX) return -75;
            references[slot]++;
            return 0;
        case SFM_FILE_RELEASE:
            if (!references[slot]) return -9;
            references[slot]--;
            return 0;
        case SFM_FILE_STAT:
            describe((uint32_t)slot, resp);
            return 0;
        case SFM_FILE_READDIR:
            if (slot) return -20;
            if (req->offset >= 2) return 0;
            describe((uint32_t)req->offset + 1, resp);
            resp->size = 5;
            sfm_copy(resp->data, req->offset ? "zero" : "null", resp->size);
            resp->result = 1;
            return 0;
        case SFM_FILE_READ:
        {
            if (!slot) return -21;
            if (req->size != sizeof(uint32_t)) return -22;
            uint32_t count;
            sfm_copy(&count, req->data, sizeof(count));
            if (count > SFM_PAYLOAD) return -22;
            if (slot == 1) return 0;
            sfm_clear(resp->data, count);
            resp->size = count;
            resp->result = count;
            return 0;
        }
        case SFM_FILE_WRITE:
            if (!slot) return -21;
            resp->result = req->size; /* Discard data; device size remains zero. */
            return 0;
        case SFM_FILE_SEEK:
            if (!slot) return -21;
            if (req->flags > 2) return -22;
            return 0; /* null/zero always report position zero. */
        case SFM_FILE_TRUNCATE: return -22;
        case SFM_FILE_REMOVE:
        case SFM_FILE_MKDIR:
        case SFM_FILE_SYMLINK: return -30;
        default: return -38;
    }
}

static int start(const sfm_api_t *api, uint32_t owner)
{
    core = api;
    owner_id = owner;
    endpoint = core->endpoint_create(owner, "dev.tmpfs", receive);
    return endpoint ? 0 : -12;
}
static void stop(void)
{
    if (endpoint) core->endpoint_destroy(owner_id, endpoint);
    endpoint = 0;
}
SFM_MODULE("dev.tmpfs", start, stop);
