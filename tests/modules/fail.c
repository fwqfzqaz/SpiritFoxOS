/* 测试 init 中途失败：必须调用 exit 并回收端点和模块分配。 */
#include "sfmod.h"
static const sfm_api_t *core;
static uint64_t endpoint;
static uint32_t owner_id;
static void *allocation;
static int start(const sfm_api_t *api, uint32_t owner)
{
    core = api;
    owner_id = owner;
    endpoint = core->endpoint_create(owner, "test.fail", NULL);
    allocation = core->alloc(128);
    return -5;
}
static void stop(void)
{
    core->free(allocation);
    if (endpoint) core->endpoint_destroy(owner_id, endpoint);
}
SFM_DEPENDS("fs.tmpfs");
SFM_MODULE("test.fail", start, stop);
