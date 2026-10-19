#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/core/block.h"
extern void test_fail_allocations(int);
static uint32_t fixture_mask;
void test_block_fixture(unsigned pass) { fixture_mask = pass == 0 ? 0 : pass == 1 ? 0x8009 : 0xffff; }
int sf_block_read(uint32_t index, sfm_block_record_t *out)
{
    if (!out || index >= SFM_BLOCK_MAX) return SFM_EINVAL;
    if (!(fixture_mask & (1u << index))) return 0;
    blkdev_t device = {.id = (uint8_t)index, .in_use = 1,
        .type = index == 15 ? BLKDEV_TYPE_RAMDISK : BLKDEV_TYPE_AHCI,
        .sector_count = 1024u + index, .sector_size = 512, .driver_data = (void *)0xdeadbeef};
    snprintf(device.name, sizeof(device.name), "disk%u", index);
    return sfm_block_describe(&device, out) ? SFM_EINVAL : 1;
}

static sfm_block_record_t item(unsigned index)
{
    sfm_block_record_t r = {.version = SFM_BLOCK_VERSION, .size = sizeof(r),
        .sector_count = 4096, .sector_size = 512, .index = index, .type = SFM_BLOCK_AHCI,
        .flags = SFM_BLOCK_GEOMETRY_UNVERIFIED};
    snprintf(r.name, sizeof(r.name), "disk%u", index);
    return r;
}

void test_block_protocol(void)
{
    uint64_t endpoint = sfm_lookup("dev.block");
    sfm_message_t *req = calloc(1, sizeof(*req)), *resp = calloc(1, sizeof(*resp));
    assert(endpoint && req && resp);
    req->flags = SFM_BLOCK_VERSION; req->type = SFM_BLOCK_ENUM;
    assert(sfm_call(endpoint, req, resp) == SFM_EAGAIN);
    req->type = SFM_BLOCK_GET;
    assert(sfm_call(endpoint, req, resp) == SFM_EAGAIN);
    req->type = SFM_BLOCK_REGISTER; req->size = sizeof(sfm_block_record_t);
    for (unsigned bad = 0; bad < 16; bad++) {
        sfm_block_record_t r = item(0);
        switch (bad) {
        case 0: r.version++; break;
        case 1: r.size--; break;
        case 2: r.snapshot = endpoint; break;
        case 3: r.index = SFM_BLOCK_MAX; break;
        case 4: r.type = 0; break;
        case 5: r.type = 4; break;
        case 6: r.sector_size = 0; break;
        case 7: r.sector_size = 513; break;
        case 8: r.sector_count = UINT64_MAX; break;
        case 9: r.flags = 2; break;
        case 10: r.reserved[0] = 1; break;
        case 11: r.name[0] = 0; break;
        case 12: memset(r.name, 'a', sizeof(r.name)); break;
        case 13: r.name[0] = '/'; break;
        case 14: r.name[15] = 'a'; break;
        case 15: r.reserved[1] = 1; break;
        }
        memcpy(req->data, &r, sizeof(r));
        assert(sfm_call(endpoint, req, resp) == SFM_EINVAL);
    }
    sfm_block_record_t r = item(0);
    memcpy(req->data, &r, sizeof(r));
    req->size--; assert(sfm_call(endpoint, req, resp) == SFM_EINVAL); req->size++;
    req->flags++; assert(sfm_call(endpoint, req, resp) == -93); req->flags--;
    req->route = 1; assert(sfm_call(endpoint, req, resp) == SFM_EINVAL); req->route = 0;
    req->handle = 1; assert(sfm_call(endpoint, req, resp) == SFM_EINVAL); req->handle = 0;
    req->offset = 1; assert(sfm_call(endpoint, req, resp) == SFM_EINVAL); req->offset = 0;
    test_fail_allocations(0); /* All catalog operations are allocation-free after init. */
    for (unsigned i = SFM_BLOCK_MAX; i-- > 0;) {
        r = item(i);
        if (i == 15) r.sector_count = 0;
        if (!i) { memset(r.name, 'z', 15); r.name[15] = 0; r.sector_count = UINT64_MAX / 512; }
        memcpy(req->data, &r, sizeof(r)); assert(!sfm_call(endpoint, req, resp));
        assert(sfm_call(endpoint, req, resp) == SFM_EEXIST);
        if (i == 15) {
            r.index = 0; /* Different slot, same name must also be rejected. */
            memcpy(req->data, &r, sizeof(r));
            assert(sfm_call(endpoint, req, resp) == SFM_EEXIST);
        }
    }
    r = item(0); r.index = SFM_BLOCK_MAX;
    memcpy(req->data, &r, sizeof(r)); assert(sfm_call(endpoint, req, resp) == SFM_EINVAL);
    req->type = SFM_BLOCK_SEAL; req->size = 1;
    assert(sfm_call(endpoint, req, resp) == SFM_EINVAL); req->size = 0;
    assert(!sfm_call(endpoint, req, resp));
    assert(sfm_call(endpoint, req, resp) == SFM_EBUSY);
    req->type = SFM_BLOCK_REGISTER; assert(sfm_call(endpoint, req, resp) == -30);
    req->type = SFM_BLOCK_ENUM;
    for (unsigned i = 0; i < SFM_BLOCK_MAX; i++) {
        req->offset = i;
        assert(!sfm_call(endpoint, req, resp) && resp->result == 1 && resp->size == sizeof(r));
        memcpy(&r, resp->data, sizeof(r));
        assert(r.index == i && r.snapshot == endpoint && !r.reserved[0] && !r.reserved[1]);
        if (i == 15) assert(!r.sector_count);
        req->type = SFM_BLOCK_GET; req->handle = endpoint;
        assert(!sfm_call(endpoint, req, resp) && resp->result == 1 && resp->offset == i);
        req->handle ^= 1ull << 32;
        assert(sfm_call(endpoint, req, resp) == SFM_ENOENT);
        req->type = SFM_BLOCK_ENUM; req->handle = 0;
    }
    req->offset = SFM_BLOCK_MAX; assert(!sfm_call(endpoint, req, resp) && !resp->size && !resp->result);
    req->offset++; assert(sfm_call(endpoint, req, resp) == SFM_EINVAL);
    req->offset = UINT64_MAX; assert(sfm_call(endpoint, req, resp) == SFM_EINVAL);
    req->type = SFM_BLOCK_GET; req->handle = endpoint;
    assert(sfm_call(endpoint, req, resp) == SFM_EINVAL);
    req->type = 0xffff; assert(sfm_call(endpoint, req, resp) == SFM_ENOSYS);
    test_fail_allocations(-1);
    free(req); free(resp);
}

void test_block_publish(void)
{
    for (int budget = 0; budget < 2; budget++) {
        test_fail_allocations(budget);
        assert(sfm_blocks_publish() == SFM_ENOMEM);
        test_fail_allocations(-1);
    }
    assert(!sfm_blocks_publish());
    assert(sfm_blocks_publish() == (fixture_mask ? -30 : SFM_EBUSY));
    sfm_message_t *req = calloc(1, sizeof(*req)), *resp = calloc(1, sizeof(*resp));
    assert(req && resp);
    req->type = SFM_BLOCK_GET; req->flags = SFM_BLOCK_VERSION;
    req->handle = sfm_lookup("dev.block");
    for (uint32_t i = 0; i < SFM_BLOCK_MAX; i++) {
        req->offset = i;
        int result = sfm_call(req->handle, req, resp);
        if (!(fixture_mask & (1u << i))) assert(result == SFM_ENOENT);
        else {
            sfm_block_record_t r; memcpy(&r, resp->data, sizeof(r));
            assert(!result && r.flags == (i == 15 ? 0 : SFM_BLOCK_GEOMETRY_UNVERIFIED));
        }
    }
    free(req); free(resp);
}
