/* Exercise real loaded registry code, including failed registration rollback. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../kernel/core/internal.h"
static sfm_message_t *req, *resp;
static uint64_t registry;
extern void test_fail_allocations(int);
static void admin(uint32_t type)
{ memset(req, 0, sizeof(*req)); req->type = type; req->flags = SFM_DEV_VERSION; }
static sfm_device_record_t record(const char *path, uint32_t type, uint32_t backend, uint64_t id)
{
    sfm_device_record_t item = {.version = SFM_DEV_VERSION, .size = sizeof(item),
        .owner = SFM_DEV_BOOT, .type = type, .backend = backend, .backend_handle = id};
    item.mode = (type == SFM_NODE_DIR ? 0040000 : type == SFM_NODE_CHARDEV ? 0020000 : 0060000) | 0755;
    strcpy(item.path, path); return item;
}
static int publish(const sfm_device_record_t *item)
{
    admin(SFM_DEV_REGISTER); req->size = sizeof(*item); memcpy(req->data, item, sizeof(*item));
    return sfm_call(registry, req, resp);
}
void test_registry(void)
{
    req = calloc(1, sizeof(*req)); resp = calloc(1, sizeof(*resp));
    registry = sfm_lookup("dev.registry"); assert(req && resp && registry);
    admin(SFM_DEV_ENUM); assert(sfm_call(registry, req, resp) == SFM_EAGAIN);
    admin(SFM_DEV_UNREGISTER); assert(sfm_call(registry, req, resp) == SFM_ENOSYS);
    admin(SFM_DEV_SEAL); assert(sfm_call(registry, req, resp) == SFM_EINVAL);
    sfm_device_record_t item = record("/dev/rejected", SFM_NODE_DIR, SFM_DEV_DIRECTORY, 0);
    item.owner = 99; assert(publish(&item) == SFM_EPERM); item.owner = SFM_DEV_BOOT;
    const char *bad[] = {"/device/no", "/dev/", "/dev/../x", "/dev/./x", "/dev/a//b", "/dev/a b"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        strcpy(item.path, bad[i]); assert(publish(&item) == SFM_EINVAL);
    }
    strcpy(item.path, "/dev/"); memset(item.path + 5, 'x', 64); item.path[69] = 0;
    assert(publish(&item) == SFM_EINVAL);
    memset(item.path, 'x', sizeof(item.path)); memcpy(item.path, "/dev/", 5);
    assert(publish(&item) == SFM_EINVAL);
    strcpy(item.path, "/dev/rejected"); item.backend = 99;
    assert(publish(&item) == SFM_EINVAL); item.backend = 0;
    item.version++; assert(publish(&item) == -93); item.version--;
    item.reserved = 1; assert(publish(&item) == SFM_EINVAL); item.reserved = 0;
    admin(SFM_DEV_REGISTER); assert(sfm_call(registry, req, resp) == SFM_EINVAL);
    req->flags++; assert(sfm_call(registry, req, resp) == -93);
    uint64_t backend = sfm_lookup("dev.tmpfs");
    for (unsigned i = 0; i < 2; i++) {
        memset(req, 0, sizeof(*req)); req->type = SFM_FS_ROOT;
        assert(!sfm_call(backend, req, resp));
        req->type = SFM_FILE_OPEN; req->handle = resp->handle; req->size = 5;
        memcpy(req->data, i ? "zero" : "null", 5);
        assert(!sfm_call(backend, req, resp));
        item = record(i ? "/dev/zero" : "/dev/null", SFM_NODE_CHARDEV, SFM_DEV_TMPFS, resp->handle);
        item.owner = SFM_DEV_SPECIAL; item.rdev = i ? 0x105 : 0x103; item.flags = SFM_DEV_RDEV_KNOWN;
        for (int budget = 0; budget < 2; budget++) {
            test_fail_allocations(budget);
            int failure = publish(&item);
            test_fail_allocations(-1);
            assert(failure == SFM_ENOMEM);
        }
        assert(!publish(&item)); assert(publish(&item) == SFM_EEXIST);
    }
    item = record("/dev/input", SFM_NODE_DIR, SFM_DEV_DIRECTORY, 0); assert(!publish(&item));
    item = record("/dev/input/event0", SFM_NODE_CHARDEV, SFM_DEV_CHAR, 128); assert(!publish(&item));
    item = record("/dev/dri", SFM_NODE_DIR, SFM_DEV_DIRECTORY, 0); assert(!publish(&item));
    item = record("/dev/dri/card0", SFM_NODE_CHARDEV, SFM_DEV_CHAR, 64); assert(!publish(&item));
    item = record("/dev/console", SFM_NODE_CHARDEV, SFM_DEV_CONSOLE, 1); assert(!publish(&item));
    item = record("/dev/sda", SFM_NODE_BLKDEV, SFM_DEV_BLOCK, 0); item.bytes = 65536; assert(!publish(&item));
    item = record("/dev/name_longer_than_thirty_two_characters_123456789", SFM_NODE_DIR, 0, 0);
    assert(!publish(&item) && publish(&item) == SFM_EEXIST);
    admin(SFM_DEV_SEAL); assert(!sfm_call(registry, req, resp));
    assert(sfm_call(registry, req, resp) == SFM_EBUSY);
    assert(publish(&item) == -30);
    admin(SFM_DEV_LOOKUP); req->size = (uint32_t)strlen(item.path) + 1;
    memcpy(req->data, item.path, req->size); assert(!sfm_call(registry, req, resp) && resp->result == 1);
    uint64_t stale = resp->handle ^ (1ull << 32);
    memset(req, 0, sizeof(*req)); req->type = SFM_FILE_STAT; req->handle = stale;
    assert(sfm_call(registry, req, resp) == -9);
    for (unsigned i = 0; i <= 10; i++) {
        admin(SFM_DEV_ENUM); req->offset = i;
        assert(!sfm_call(registry, req, resp));
        assert(i == 10 ? !resp->result : resp->result == 1 && resp->size == sizeof(item));
    }
    admin(SFM_DEV_ENUM); req->offset = 11; assert(sfm_call(registry, req, resp) == SFM_EINVAL);
    admin(SFM_DEV_LOOKUP); memcpy(req->data, "/dev/dri/card0", sizeof("/dev/dri/card0"));
    req->size = sizeof("/dev/dri/card0") + 1;
    /* Embedded trailing NULs must fail; exact length works. */
    assert(sfm_call(registry, req, resp) == SFM_EINVAL);
    req->size = sizeof("/dev/dri/card0");
    assert(!sfm_call(registry, req, resp));
    uint64_t legacy = resp->handle;
    memset(req, 0, sizeof(*req)); req->type = SFM_FILE_READ; req->handle = legacy;
    assert(sfm_call(registry, req, resp) == -95);
    admin(SFM_DEV_LOOKUP); req->size = sizeof("/dev/null"); memcpy(req->data, "/dev/null", req->size);
    assert(!sfm_call(registry, req, resp));
    uint64_t null = resp->handle;
    memset(req, 0, sizeof(*req)); req->type = SFM_FILE_RETAIN; req->handle = null;
    assert(!sfm_call(registry, req, resp));
    req->type = SFM_FILE_RELEASE;
    test_fail_allocations(0);
    int released = sfm_call(registry, req, resp);
    test_fail_allocations(-1);
    assert(!released && sfm_call(registry, req, resp) == -9);
    free(req); free(resp);
}
void test_registry_capacity(void)
{
    req = calloc(1, sizeof(*req)); resp = calloc(1, sizeof(*resp));
    registry = sfm_lookup("dev.registry"); assert(req && resp && registry);
    for (unsigned i = 1; i <= SFM_DEV_MAX; i++) {
        char path[64]; snprintf(path, sizeof(path), "/dev/item%u", i);
        sfm_device_record_t item = record(path, SFM_NODE_DIR, SFM_DEV_DIRECTORY, 0);
        assert(publish(&item) == (i == SFM_DEV_MAX ? SFM_ENOSPC : 0));
    }
    free(req); free(resp);
}
