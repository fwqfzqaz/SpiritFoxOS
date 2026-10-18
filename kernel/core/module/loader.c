/* 启动包验证、依赖排序与生命周期。装卸仅由串行启动/管理入口调用。 */
#include "../internal.h"

#define PACKAGE_MAGIC 0x4b504653u
#define PACKAGE_LIMIT (16u * 1024u * 1024u)
typedef struct package_header
{
    uint32_t magic, abi, count, size;
} package_header_t;
typedef struct package_item
{
    char name[32];
    uint64_t offset, size;
} package_item_t;
typedef struct loaded_module
{
    sfm_image_t image;
    int started;
} loaded_module_t;

static loaded_module_t modules[SFM_MAX_MODULES];
static size_t module_count;
static uint32_t order[SFM_MAX_MODULES];
static size_t started_count;

static const sfm_api_t api = {
    .abi = SFM_ABI, .size = sizeof(sfm_api_t),
    .alloc = sf_alloc, .free = sf_free, .log = sf_log,
    .endpoint_create = sfm_endpoint_create, .endpoint_destroy = sfm_endpoint_destroy,
    .lookup = sfm_lookup, .call = sfm_call, .send = sfm_send, .recv = sfm_recv
};

static int find_module(const char *name)
{
    for (size_t i = 0; i < module_count; i++)
        if (modules[i].image.allocation && sfm_name_equal(name, modules[i].image.meta.name))
            return (int)i;
    return -1;
}

static int dependency_ready(size_t index, const uint8_t *ready)
{
    const sfm_image_t *image = &modules[index].image;
    for (size_t i = 0; i < image->dep_count; i++)
    {
        int dep = find_module(image->deps[i]);
        if (dep < 0) return SFM_ENOENT;
        if (!ready[dep]) return 0;
    }
    return 1;
}

static void rollback(void)
{
    while (started_count)
    {
        size_t index = order[--started_count];
        modules[index].image.entry.exit();
        sfm_owner_remove((uint32_t)index + 1);
        modules[index].started = 0;
    }
    for (size_t i = 0; i < module_count; i++) sfm_elf_free(&modules[i].image);
    module_count = 0;
}

int sfm_package_load(const void *data, size_t size)
{
    if (module_count) return SFM_EBUSY;
    if (!data || size < sizeof(package_header_t) || size > PACKAGE_LIMIT)
        return SFM_ENOEXEC;
    package_header_t header;
    sfm_copy(&header, data, sizeof(header));
    if (header.magic != PACKAGE_MAGIC || header.abi != SFM_ABI || header.size != size ||
        !header.count || header.count > SFM_MAX_MODULES) return SFM_ENOEXEC;
    size_t directory_end = sizeof(header) + header.count * sizeof(package_item_t);
    if (directory_end > size) return SFM_ENOEXEC;
    package_item_t entries[SFM_MAX_MODULES];
    sfm_copy(entries, (const uint8_t *)data + sizeof(header), directory_end - sizeof(header));
    int result = SFM_ENOEXEC;
    for (size_t i = 0; i < header.count; i++)
    {
        result = SFM_ENOEXEC;
        const package_item_t *item = &entries[i];
        if (!sfm_name_valid(item->name, 32) || item->offset < directory_end ||
            item->offset > size || !item->size || item->size > size - item->offset)
            goto fail;
        for (size_t j = 0; j < i; j++)
            if (sfm_name_equal(item->name, entries[j].name) ||
                (item->offset < entries[j].offset + entries[j].size &&
                 entries[j].offset < item->offset + item->size)) goto fail;
        module_count = i + 1;
        result = sfm_elf_load((const uint8_t *)data + item->offset, item->size,
                              &modules[i].image);
        if (result) goto fail;
        if (!sfm_name_equal(item->name, modules[i].image.meta.name))
        {
            result = SFM_ENOEXEC;
            goto fail;
        }
    }
    /* 先验证整个依赖图，再执行任何入口。Kahn 排序，优先级只打破并列。 */
    uint8_t ready[SFM_MAX_MODULES] = {0};
    size_t planned = 0;
    while (planned < module_count)
    {
        int chosen = -1;
        for (size_t i = 0; i < module_count; i++)
        {
            if (ready[i]) continue;
            result = dependency_ready(i, ready);
            if (result < 0) goto fail;
            if (result && (chosen < 0 || modules[i].image.meta.priority <
                          modules[chosen].image.meta.priority)) chosen = (int)i;
        }
        if (chosen < 0) { result = SFM_EINVAL; goto fail; }
        ready[chosen] = 1;
        order[planned++] = (uint32_t)chosen;
    }
    for (size_t i = 0; i < module_count; i++)
    {
        uint32_t index = order[i];
        sfm_owner_enable(index + 1, 1);
        /* init 失败也调用 exit，让模块清理自身的部分分配。 */
        modules[index].started = 1;
        started_count++;
        result = modules[index].image.entry.init(&api, index + 1);
        if (result) goto fail;
        sf_log("[sfmod] ready: ");
        sf_log(modules[index].image.meta.name);
        sf_log("\n");
    }
    return 0;
fail:
    rollback();
    return result;
}

int sfm_module_unload(const char *name)
{
    if (!sfm_name_valid(name, 32)) return SFM_EINVAL;
    int index = find_module(name);
    if (index < 0 || !modules[index].started) return SFM_ENOENT;
    for (size_t i = 0; i < module_count; i++)
        if (modules[i].started)
            for (size_t d = 0; d < modules[i].image.dep_count; d++)
                if (sfm_name_equal(name, modules[i].image.deps[d])) return SFM_EBUSY;
    if (sfm_owner_busy((uint32_t)index + 1)) return SFM_EBUSY;
    modules[index].image.entry.exit();
    sfm_owner_remove((uint32_t)index + 1);
    modules[index].started = 0;
    sfm_elf_free(&modules[index].image);
    int any = 0;
    for (size_t i = 0; i < module_count; i++) any |= modules[i].started;
    if (!any) module_count = started_count = 0;
    return 0;
}
