#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/core/block.h"
extern void test_fail_allocations(int);
static uint32_t fixture_mask;
static unsigned fixture_pass;
static int frozen, fail_dma, fail_io, nested_io;
static unsigned raw_calls, next_page;
static _Alignas(4096) uint8_t dma_pages[SFM_BLOCK_MAX][4096];
void test_block_fixture(unsigned pass) { fixture_pass = pass; frozen = 0; fixture_mask = pass == 0 ? 0 : pass == 1 ? 0x8009 : 0xffff; }
int sf_block_freeze(void) { if (frozen) return SFM_EBUSY; frozen = 1; return 0; }
int sf_block_frozen(void) { return frozen; }
void *sf_block_dma_page(void) { if (fail_dma) return NULL; assert(next_page < SFM_BLOCK_MAX); return dma_pages[next_page++]; }
int sf_block_raw_read(uint32_t index, uint64_t lba, uint32_t count, void *data)
{
    raw_calls++;
    assert(frozen && index < SFM_BLOCK_MAX && (fixture_mask & (1u << index)) &&
           count && count <= 4096 / (index == 15 ? 4096 : 512) && !((uintptr_t)data & 4095));
    if (nested_io) {
        uint8_t byte;
        assert(sf_block_read_sectors(index, lba, count, &byte, 4096) == SFM_EBUSY);
    }
    unsigned bytes = count * (index == 15 ? 4096 : 512);
    if (fail_io == -5) bytes /= 2; /* Partial DMA before the error. */
    for (unsigned i = 0; i < bytes; i++) ((uint8_t *)data)[i] = (uint8_t)(index + lba + i);
    return fail_io;
}
int sf_block_read(uint32_t index, sfm_block_record_t *out)
{
    if (!out || index >= SFM_BLOCK_MAX) return SFM_EINVAL;
    if (!(fixture_mask & (1u << index))) return 0;
    blkdev_t device = {.id = (uint8_t)index, .in_use = 1,
        .type = index >= 14 ? BLKDEV_TYPE_RAMDISK : BLKDEV_TYPE_AHCI,
        .geometry_source = index >= 14 ? BLKDEV_GEOMETRY_SOFTWARE : index == 3 ? BLKDEV_GEOMETRY_UNKNOWN : BLKDEV_GEOMETRY_ATA,
        .sector_count = index == 14 ? 0 : 1024u + index, .sector_size = index == 15 ? 4096 : 512,
        .driver_data = (void *)0xdeadbeef};
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
    for (unsigned bad = 0; bad < 20; bad++) {
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
        case 10: r.reserved = 1; break;
        case 11: r.name[0] = 0; break;
        case 12: memset(r.name, 'a', sizeof(r.name)); break;
        case 13: r.name[0] = '/'; break;
        case 14: r.name[15] = 'a'; break;
        case 15: r.geometry_source = 3; break;
        case 16: r.geometry_source = SFM_BLOCK_GEOMETRY_SOFTWARE; r.flags = 0; break;
        case 17: r.flags = 0; break;
        case 18: r.type = SFM_BLOCK_RAMDISK; r.geometry_source = SFM_BLOCK_GEOMETRY_SOFTWARE; break;
        case 19: r.geometry_source = SFM_BLOCK_GEOMETRY_ATA; r.flags = 0; r.type = SFM_BLOCK_IDE; break;
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
        assert(r.index == i && r.snapshot == endpoint && !r.reserved);
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
    req->type = SFM_BLOCK_WRITE; assert(sfm_call(endpoint, req, resp) == -30);
    test_fail_allocations(-1);
    free(req); free(resp);
}

static void check_read(uint64_t endpoint, uint32_t index, uint64_t lba, uint32_t count, int expected)
{
    sfm_message_t *req = calloc(1, sizeof(*req)), *resp = malloc(sizeof(*resp));
    assert(req && resp); memset(resp, 0xa5, sizeof(*resp));
    sfm_block_read_t read = {.index = index, .count = count};
    req->type = SFM_BLOCK_READ; req->flags = SFM_BLOCK_VERSION; req->handle = endpoint;
    req->offset = lba; req->size = sizeof(read); memcpy(req->data, &read, sizeof(read));
    unsigned before = raw_calls;
    assert(sfm_call(sfm_lookup("dev.block"), req, resp) == expected);
    if (expected) {
        assert(!resp->size && resp->result == expected);
        for (unsigned i = 0; i < SFM_PAYLOAD; i++) assert(!resp->data[i]);
        if (expected != -5 && expected != SFM_EBUSY) assert(before == raw_calls);
    } else {
        unsigned bytes = count * (index == 15 ? 4096 : 512);
        assert(resp->result == bytes && resp->size == bytes && resp->handle == endpoint && resp->offset == lba);
        for (unsigned i = 0; i < bytes; i++) assert(resp->data[i] == (uint8_t)(index + lba + i));
        for (unsigned i = bytes; i < SFM_PAYLOAD; i++) assert(!resp->data[i]);
    }
    free(req); free(resp);
}

static void test_reads(void)
{
    uint64_t endpoint = sfm_lookup("dev.block");
    if (!fixture_mask) { check_read(endpoint, 0, 0, 1, SFM_ENOENT); return; }
    check_read(endpoint, 0, 0, 1, 0);
    check_read(endpoint, 15, 1038, 1, 0); /* One 4096-byte software sector, final LBA. */
    check_read(endpoint, 3, 0, 1, -19); /* Unknown capacity cannot be used. */
    check_read(endpoint ^ (1ull << 32), 0, 0, 1, SFM_ENOENT);
    check_read(endpoint, SFM_BLOCK_MAX, 0, 1, SFM_EINVAL);
    check_read(endpoint, 0, 0, 0, SFM_EINVAL);
    check_read(endpoint, 0, 0, 9, SFM_EINVAL);
    check_read(endpoint, 0, 1024, 1, SFM_EINVAL);
    check_read(endpoint, 0, UINT64_MAX, 2, SFM_EINVAL);
    check_read(endpoint, 15, 0, 2, SFM_EINVAL);
    if (fixture_pass != 2) { check_read(endpoint, 1, 0, 1, SFM_ENOENT); return; }
    check_read(endpoint, 14, 0, 1, SFM_EINVAL); /* Empty known software device. */
    fail_dma = 1; check_read(endpoint, 1, 0, 1, SFM_ENOMEM); fail_dma = 0;
    nested_io = 1; check_read(endpoint, 1, 1017, 8, 0); nested_io = 0;
    test_fail_allocations(0); check_read(endpoint, 1, 1024, 1, 0); test_fail_allocations(-1);
    fail_io = -5; check_read(endpoint, 2, 0, 1, -5); fail_io = 0;
    unsigned before = raw_calls;
    check_read(endpoint, 2, 0, 1, -5); assert(raw_calls == before); /* Quarantined, not retried. */
    fail_io = -16; check_read(endpoint, 5, 0, 1, SFM_EBUSY); fail_io = 0;
    check_read(endpoint, 5, 0, 1, 0);
    fail_io = 1; check_read(endpoint, 6, 0, 1, -5); fail_io = 0;
    sfm_message_t *request = calloc(1, sizeof(*request)), *response = calloc(1, sizeof(*response));
    assert(request && response);
    request->type = SFM_BLOCK_READ; request->flags = SFM_BLOCK_VERSION;
    request->handle = endpoint; request->size = sizeof(sfm_block_read_t);
    sfm_block_read_t invalid = {.index = 1, .count = 1, .reserved = 1};
    memcpy(request->data, &invalid, sizeof(invalid));
    assert(sfm_call(endpoint, request, response) == SFM_EINVAL);
    invalid.reserved = 0; memcpy(request->data, &invalid, sizeof(invalid));
    request->size--; assert(sfm_call(endpoint, request, response) == SFM_EINVAL); request->size++;
    request->flags--; assert(sfm_call(endpoint, request, response) == -93); request->flags++;
    request->route = 1; assert(sfm_call(endpoint, request, response) == SFM_EINVAL);
    free(request); free(response);
    uint8_t result[4096]; memset(result, 0x7c, sizeof(result));
    assert(sf_block_read_sectors(1, 0, 1, result, 511) == SFM_EINVAL);
    assert(sf_block_read_sectors(1, 0, 1, result, 4097) == SFM_EINVAL);
    assert(sf_block_read_sectors(1, UINT64_MAX, 1, result, 4096) == SFM_EINVAL);
    assert(sf_block_read_sectors(3, 0, 1, result, 4096) == -19);
    fail_io = -5;
    assert(sf_block_read_sectors(7, 0, 1, result, 4096) == -5);
    fail_io = 0;
    for (unsigned i = 0; i < sizeof(result); i++) assert(result[i] == 0x7c);
}

void test_block_publish(void)
{
    uint8_t byte;
    assert(sf_block_read_sectors(0, 0, 1, &byte, 1) == SFM_EAGAIN);
    for (int budget = 0; budget < 2; budget++) {
        test_fail_allocations(budget);
        assert(sfm_blocks_publish() == SFM_ENOMEM);
        test_fail_allocations(-1);
    }
    assert(!sfm_blocks_publish());
    test_reads();
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
            assert(!result && r.flags == (i == 3 ? SFM_BLOCK_GEOMETRY_UNVERIFIED : 0));
        }
    }
    free(req); free(resp);
}
