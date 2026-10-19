/* Fixed boot mounts with explicit backend routes; handles are backend-local. */
#include "sfmod.h"
static const sfm_api_t *core;
static uint32_t owner_id;
static uint64_t endpoint, backends[SFM_ROUTE_COUNT];
static struct mount { const char *path; uint64_t root; uint32_t route, instance; } mounts[] = {
    {"/modules", 0, SFM_ROUTE_TMPFS, 0}, {"/tmp", 0, SFM_ROUTE_TMPFS, 1},
    {"/run", 0, SFM_ROUTE_TMPFS, 2}, {"/dev", 0, SFM_ROUTE_DEVFS, 0}
};

static int forward(uint64_t route, const sfm_message_t *req, sfm_message_t *resp)
{
    if (route >= SFM_ROUTE_COUNT) return -22;
    int result = core->call(backends[route], req, resp);
    resp->route = route;
    return result;
}

static int receive(const sfm_message_t *req, sfm_message_t *resp)
{
    if (req->type == SFM_FS_ROOT) return -38;
    if (req->route >= SFM_ROUTE_COUNT) return -22;
    if (req->type != SFM_FILE_OPEN && req->type != SFM_FILE_MKDIR)
        return forward(req->route, req, resp);
    if (!req->size || req->size > SFM_FILE_PATH || req->data[req->size - 1]) return -22;
    for (size_t i = 0; i + 1 < req->size; i++) if (!req->data[i]) return -22;
    /* 带目录句柄的相对操作已经绑定挂载；不再重新解释路径。 */
    if (req->data[0] != '/') return forward(req->route, req, resp);
    struct mount *selected = NULL;
    size_t skip = 0;
    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++)
    {
        size_t length = 0;
        while (mounts[i].path[length]) length++;
        if (length >= req->size || length < skip) continue;
        size_t j = 0;
        while (j < length && req->data[j] == (uint8_t)mounts[i].path[j]) j++;
        if (j == length && (!req->data[j] || req->data[j] == '/'))
        { selected = &mounts[i]; skip = length; }
    }
    if (!selected) return -2;
    if (req->data[skip] == '/') skip++;
    sfm_message_t *forward = core->alloc(sizeof(*forward));
    if (!forward) return -12;
    sfm_copy(forward, req, sizeof(*forward));
    forward->handle = selected->root;
    forward->route = selected->route;
    forward->size -= (uint32_t)skip;
    sfm_copy(forward->data, req->data + skip, forward->size);
    int result = core->call(backends[selected->route], forward, resp);
    resp->route = selected->route;
    core->free(forward);
    return result;
}

static int start(const sfm_api_t *api, uint32_t owner)
{
    core = api;
    owner_id = owner;
    backends[SFM_ROUTE_TMPFS] = core->lookup("fs.tmpfs");
    backends[SFM_ROUTE_DEVFS] = core->lookup("dev.registry");
    if (!backends[0] || !backends[1]) return -2;
    sfm_message_t *req = core->alloc(sizeof(*req)), *resp = core->alloc(sizeof(*resp));
    int result = -12;
    if (!req || !resp) goto done;
    sfm_clear(req, sizeof(*req));
    req->type = SFM_FS_ROOT;
    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++)
    {
        req->offset = mounts[i].instance;
        result = core->call(backends[mounts[i].route], req, resp);
        if (result) goto done;
        mounts[i].root = resp->handle;
    }
    endpoint = core->endpoint_create(owner, "fs.vfs", receive);
    result = endpoint ? 0 : -12;
done:
    core->free(req);
    core->free(resp);
    return result;
}

static void stop(void)
{
    if (endpoint) core->endpoint_destroy(owner_id, endpoint);
    endpoint = 0;
}
SFM_DEPENDS("fs.tmpfs", "dev.registry");
SFM_MODULE("fs.vfs", start, stop);
