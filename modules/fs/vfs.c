/* 固定启动挂载表：独立 tmpfs 根用于 /modules、/tmp、/run。 */
#include "sfmod.h"
static const sfm_api_t *core;
static uint32_t owner_id;
static uint64_t endpoint, backend;
static struct mount { const char *path; uint64_t root; } mounts[SFM_FS_INSTANCES] = {
    {"/modules", 0}, {"/tmp", 0}, {"/run", 0}
};

static int receive(const sfm_message_t *req, sfm_message_t *resp)
{
    if (req->type == SFM_FS_ROOT) return -38;
    if (req->type != SFM_FILE_OPEN && req->type != SFM_FILE_MKDIR)
        return core->call(backend, req, resp);
    if (!req->size || req->size > SFM_FILE_PATH || req->data[req->size - 1]) return -22;
    for (size_t i = 0; i + 1 < req->size; i++) if (!req->data[i]) return -22;
    /* 带目录句柄的相对操作已经绑定挂载；不再重新解释路径。 */
    if (req->data[0] != '/') return core->call(backend, req, resp);
    struct mount *selected = NULL;
    size_t skip = 0;
    for (size_t i = 0; i < SFM_FS_INSTANCES; i++)
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
    forward->size -= (uint32_t)skip;
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
    sfm_message_t *req = core->alloc(sizeof(*req)), *resp = core->alloc(sizeof(*resp));
    int result = -12;
    if (!req || !resp) goto done;
    sfm_clear(req, sizeof(*req));
    req->type = SFM_FS_ROOT;
    for (size_t i = 0; i < SFM_FS_INSTANCES; i++)
    {
        req->offset = i;
        result = core->call(backend, req, resp);
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
SFM_DEPENDS("fs.tmpfs");
SFM_MODULE("fs.vfs", start, stop);
