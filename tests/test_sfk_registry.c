#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pkgmgr.h"

enum { H = 1084, R = 392, P = 164, SIZE = H + P + 64 * (R + 8) + 4096 + 16 };
static unsigned live, checks;
static int budget = -1;
void *kcalloc(size_t n, size_t size)
{
    if (!budget) return NULL;
    if (budget > 0) --budget;
    void *p = calloc(n, size); if (p) ++live; return p;
}
void kfree(void *p) { if (p) { assert(live); --live; free(p); } }
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v)
{ for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static size_t fixture(uint8_t *p, unsigned count, unsigned length, unsigned permissions)
{
    memset(p, 0, SIZE);
    put32(p, SFK_MAGIC); put32(p + 4, 1);
    memcpy(p + 8, "org.spiritfox.registry", sizeof("org.spiritfox.registry"));
    put32(p + 776, permissions); put32(p + 780, count);
    put64(p + 812, H);
    size_t offset = H + permissions * P;
    for (unsigned i = 0; i < count; ++i) {
        memcpy(p + offset, "HKEY_USER/Test", 15);
        memcpy(p + offset + 256, "value", 6);
        put32(p + offset + 384, 0x10203040U + i);
        put32(p + offset + 388, length);
        for (unsigned j = 0; j < length; ++j) p[offset + R + j] = (uint8_t)(i + j);
        offset += R + length;
    }
    put64(p + 820, offset - H);
    put64(p + 796, SIZE - 16); put64(p + 804, 16);
    return offset;
}
static void reject(const uint8_t *p)
{
    sfk_reg_entry_t sentinel, *headers = &sentinel;
    int count = 123;
    if (sfk_get_reg_entries(p, SIZE, &headers, &count) >= 0) {
        fprintf(stderr, "accepted malformed registry records (check %u)\n", checks);
        abort();
    }
    assert(headers == &sentinel && count == 123 && !live);
#ifndef LEGACY_ONLY
    sfk_reg_view_t sentinel_view, *views = &sentinel_view;
    assert(sfk_get_reg_views(p, SIZE, &views, &count) < 0);
    assert(views == &sentinel_view && count == 123 && !live);
#endif
    ++checks;
}
int main(void)
{
    uint8_t storage[SIZE + 1], *p = storage + 1;
    fixture(p, 1, 7, 0); put64(p + 820, R + 6); reject(p);
#ifndef LEGACY_ONLY
    for (unsigned permissions = 0; permissions <= 1; ++permissions) {
        for (unsigned length = 0; length < 2 * (R + 7); ++length) {
            fixture(p, 2, 7, permissions);
            put64(p + 820, permissions * P + length); reject(p);
        }
    }
    for (unsigned count = 0; count <= 64; ++count) {
        fixture(p, count, 7, 1);
        sfk_reg_view_t *views = NULL; int total = -1;
        assert(sfk_get_reg_views(p, SIZE, &views, &total) == 0 && total == (int)count);
        for (unsigned i = 0; i < count; ++i) {
            assert(views[i].entry.value_type == 0x10203040U + i);
            assert(views[i].entry.value_size == 7);
            assert(views[i].value_data == p + H + P + i * (R + 7) + R);
            for (unsigned j = 0; j < 7; ++j)
                assert(((const uint8_t *)views[i].value_data)[j] == (uint8_t)(i + j));
        }
        kfree(views); assert(!live); ++checks;
    }
    fixture(p, 1, 0, 0);
    sfk_reg_view_t *views = NULL; int count = -1;
    assert(sfk_get_reg_views(p, SIZE, &views, &count) == 0 && count == 1);
    kfree(views);
    fixture(p, 1, 4096, 0);
    assert(sfk_get_reg_views(p, SIZE, &views, &count) == 0 && views[0].entry.value_size == 4096);
    kfree(views);
    fixture(p, 1, 7, 0); put32(p + 780, 65); reject(p);
    fixture(p, 1, 7, 0); put32(p + 780, UINT32_MAX); reject(p);
    fixture(p, 1, 7, 0); put32(p + H + 388, UINT32_MAX); reject(p);
    fixture(p, 1, 7, 0); put32(p + H + 388, 4097); reject(p);
    fixture(p, 2, 7, 0); memset(p + H + R + 7, 'x', 256); reject(p);
    fixture(p, 2, 7, 0); memset(p + H + R + 7 + 256, 'x', 128); reject(p);
    fixture(p, 1, 7, 0); p[H] = 0; reject(p);
    fixture(p, 1, 7, 1); memset(p + H + 4, 'x', 32); reject(p);
    fixture(p, 1, 7, 0); put64(p + 812, 0); put64(p + 820, 0); reject(p);
    fixture(p, 1, 7, 0);
    for (int fail = 0; fail <= 1; ++fail) {
        budget = fail;
        sfk_reg_entry_t sentinel, *headers = &sentinel; count = 123;
        assert(sfk_get_reg_entries(p, SIZE, &headers, &count) < 0);
        assert(headers == &sentinel && count == 123 && !live);
    }
    budget = -1;
    sfk_reg_entry_t *headers = NULL;
    assert(sfk_get_reg_entries(p, SIZE, &headers, &count) == 0 && count == 1);
    assert(headers[0].value_size == 7); kfree(headers); assert(!live);
    assert(sfk_get_reg_views(p, SIZE, NULL, &count) < 0);
    assert(sfk_get_reg_views(p, SIZE, &views, NULL) < 0);
    assert(sfk_get_reg_views(NULL, SIZE, &views, &count) < 0);
    printf("SFK registry tests passed: %u cases, variable values, bounded views, quota, OOM\n", checks);
#endif
    return 0;
}
