/* 示例服务：独立 .sfmod，经核心 API 注册 PING 处理器。 */
#include "sfmod.h"
static const sfm_api_t *core;
static uint32_t owner_id;
static uint64_t endpoint;

static int receive(const sfm_message_t *request, sfm_message_t *response)
{
    if (request->type != SFM_PING) return -38;
    response->result = 0;
    response->size = request->size;
    sfm_copy(response->data, request->data, request->size);
    return 0;
}

static int start(const sfm_api_t *api, uint32_t owner)
{
    core = api;
    owner_id = owner;
    endpoint = core->endpoint_create(owner, "demo.hello", receive);
    if (!endpoint) return -12;
    core->log("[sfmod] hello from relocated module\n");
    return 0;
}

static void stop(void)
{
    if (endpoint) core->endpoint_destroy(owner_id, endpoint);
    endpoint = 0;
}
SFM_MODULE("demo.hello", start, stop);
