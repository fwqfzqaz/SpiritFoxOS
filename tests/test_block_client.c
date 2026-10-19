/* Production client + real endpoint dispatcher; no physical/raw fallback. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/core/internal.h"
#include "block_client.h"
extern void test_fail_allocations(int);
static uint64_t endpoint;
static unsigned mode, calls, fail_call;
static int injected_error;
static sf_block_client_t disk;
static int reply(const sfm_message_t *req, sfm_message_t *resp)
{
    assert(req->flags == SFM_BLOCK_VERSION && !req->route && req->handle == endpoint);
    resp->handle = endpoint;
    if (req->type == SFM_BLOCK_GET) {
        assert(req->offset == 4 && !req->size);
        sfm_block_record_t r = {.version = SFM_BLOCK_VERSION, .size = sizeof(r),
            .snapshot = endpoint, .sector_count = 1024, .sector_size = 512,
            .index = 4, .type = SFM_BLOCK_AHCI, .geometry_source = SFM_BLOCK_GEOMETRY_ATA};
        if (mode == 20) r.snapshot++;
        if (mode == 21) r.sector_size = 8192;
        if (mode == 22) r.flags = SFM_BLOCK_GEOMETRY_UNVERIFIED;
        if (mode == 23) r.index++;
        if (mode == 24) r.sector_count = UINT64_MAX;
        if (mode == 25) r.geometry_source = SFM_BLOCK_GEOMETRY_SOFTWARE;
        if (mode == 26) r.version--;
        resp->result = 1; resp->offset = 1; resp->size = sizeof(r);
        memcpy(resp->data, &r, sizeof(r)); return 0;
    }
    assert(req->type == SFM_BLOCK_READ && req->size == sizeof(sfm_block_read_t));
    sfm_block_read_t r; memcpy(&r, req->data, sizeof(r));
    assert(r.index == 4 && r.count && r.count <= 8 && !r.reserved);
    calls++;
    if (fail_call == calls) return injected_error;
    if (mode == 9) {
        uint8_t buffer[512]; memset(buffer, 0xa5, sizeof(buffer));
        assert(sf_block_client_read(&disk, 0, 1, buffer, sizeof(buffer)) == SFM_EBUSY);
        for (unsigned i = 0; i < sizeof(buffer); i++) assert(!buffer[i]);
    }
    resp->offset = req->offset; resp->size = r.count * 512; resp->result = resp->size;
    for (unsigned i = 0; i < resp->size; i++) resp->data[i] = (uint8_t)((req->offset * 512 + i) % 251 + 1);
    switch (mode) {
    case 1: resp->size--; break;
    case 2: resp->size++; break;
    case 3: resp->result--; break;
    case 4: resp->handle++; break;
    case 5: resp->offset++; break;
    case 6: resp->size = SFM_PAYLOAD + 1; break;
    case 7: resp->result = -5; break;
    case 8: resp->route = 1; break;
    }
    return 0;
}
void test_block_client(void)
{
    sf_block_client_t saved; memset(&saved, 0xa5, sizeof(saved)); disk = saved;
    assert(sf_block_client_open(4, &disk) == SFM_ENOENT && !memcmp(&disk, &saved, sizeof(disk)));
    sfm_owner_enable(SFM_CORE_OWNER, 1);
    endpoint = sfm_endpoint_create(SFM_CORE_OWNER, "dev.block", reply); assert(endpoint);
    for (mode = 20; mode <= 26; mode++) {
        assert(sf_block_client_open(4, &disk) == -5 && !memcmp(&disk, &saved, sizeof(disk)));
    }
    mode = 0;
    for (int budget = 0; budget < 2; budget++) {
        test_fail_allocations(budget);
        assert(sf_block_client_open(4, &disk) == SFM_ENOMEM);
        test_fail_allocations(-1);
        assert(!memcmp(&disk, &saved, sizeof(disk)));
    }
    assert(!sf_block_client_open(4, &disk));
    uint8_t *buffer = malloc(65536 + 2); assert(buffer);
    for (unsigned sectors = 1; sectors <= 128; sectors++) {
        memset(buffer, 0xa5, 65538); calls = 0;
        assert(!sf_block_client_read(&disk, 896, sectors, buffer + 1, 65536));
        assert(calls == (sectors + 7) / 8);
        for (unsigned i = 0; i < sectors * 512; i++) assert(buffer[i+1] == (uint8_t)((896 * 512 + i) % 251 + 1));
        assert(buffer[0] == 0xa5 && buffer[sectors * 512 + 1] == 0xa5);
    }
    for (mode = 1; mode <= 9; mode++) {
        memset(buffer, 0xa5, 8192); calls = 0;
        int result = sf_block_client_read(&disk, 0, 16, buffer, 8192);
        if (mode == 9) assert(!result && calls == 2);
        else { assert(result < 0 && calls == 1); for (unsigned i = 0; i < 8192; i++) assert(!buffer[i]); }
    }
    mode = 0;
    int errors[] = {SFM_EBUSY, SFM_EAGAIN, SFM_ENOENT, -5};
    for (unsigned e = 0; e < sizeof(errors)/sizeof(errors[0]); e++) {
        for (fail_call = 1; fail_call <= 2; fail_call++) {
            injected_error = errors[e]; calls = 0; memset(buffer, 0xa5, 8192);
            assert(sf_block_client_read(&disk, 0, 16, buffer, 8192) == errors[e] && calls == fail_call);
            for (unsigned i = 0; i < 8192; i++) assert(!buffer[i]);
        }
    }
    fail_call = 0;
    for (int budget = 0; budget < 2; budget++) {
        test_fail_allocations(budget); memset(buffer, 0xa5, 8192);
        assert(sf_block_client_read(&disk, 0, 16, buffer, 8192) == SFM_ENOMEM);
        test_fail_allocations(-1);
        for (unsigned i = 0; i < 8192; i++) assert(!buffer[i]);
    }
    calls = 0;
    assert(sf_block_client_read(&disk, UINT64_MAX, 1, buffer, 8192) == SFM_EINVAL && !calls);
    assert(sf_block_client_read(&disk, 0, 129, buffer, 65536) == SFM_EINVAL && !calls);
    assert(sf_block_client_read(&disk, 0, 1, buffer, 511) == SFM_EINVAL && !calls);
    assert(!sfm_endpoint_destroy(SFM_CORE_OWNER, endpoint));
    /* Reuse the old output after endpoint destruction: sfm_call returns before
     * clearing its response, but the client must never copy that stale data. */
    memset(buffer, 0xa5, 8192);
    assert(sf_block_client_read(&disk, 0, 16, buffer, 8192) == SFM_ENOENT);
    for (unsigned i = 0; i < 8192; i++) assert(!buffer[i]);
    free(buffer);
}
