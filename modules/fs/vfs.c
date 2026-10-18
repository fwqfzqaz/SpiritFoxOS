/* 消息 VFS：首个挂载点 /modules -> fs.tmpfs。原 syscall VFS 保持兼容。 */
#include "sfmod.h"
static const sfm_api_t *core;
static uint32_t owner_id;
static uint64_t endpoint, backend;

static int receive(const sfm_message_t *req, sfm_message_t *resp)
{
    if (req->type != SFM_FILE_OPEN) return core->call(backend, req, resp);
    static const char prefix[] = "/modules/";
    const size_t skip = sizeof(prefix) - 1;
    if (req->size <= skip || req->size > skip + 32) return -22;
    for (size_t i = 0; i < skip; i++)
        if (req->data[i] != (uint8_t)prefix[i]) return -2;
    sfm_message_t *forward = core->alloc(sizeof(*forward));
    if (!forward) return -12;
    sfm_copy(forward, req, sizeof(*forward));
    forward->size -= skip;
    sfm_copy(forward->data, req->data + skip, forward->size);
    int result = core->call(backend, forward, resp);
    core->free(forward);
    return result;
}

static int start(const sfm_api_t *api, uint32_t owner)
{
    core = api;
    owner_id = owner;
    backend = core->lookup("fs.tmpfs");
    if (!backend) return -2;
    endpoint = core->endpoint_create(owner, "fs.vfs", receive);
    return endpoint ? 0 : -12;
}

static void stop(void)
{
    if (endpoint) core->endpoint_destroy(owner_id, endpoint);
    endpoint = 0;
}
SFM_DEPENDS("fs.tmpfs");
SFM_MODULE("fs.vfs", start, stop);
