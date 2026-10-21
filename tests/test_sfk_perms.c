#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pkgmgr.h"

enum { HEADER = 1084, RECORD = 164, CAPACITY = 32,
       PACKAGE = HEADER + CAPACITY * RECORD + 16 };
static unsigned checks;

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static void fixture(uint8_t *p, unsigned count)
{
    memset(p, 0, PACKAGE);
    p[0] = 0x21; p[1] = 0x4b; p[2] = 0x46; p[3] = 0x53; p[4] = 1;
    memcpy(p + 8, "org.spiritfox.perms", sizeof("org.spiritfox.perms"));
    put32(p + 776, count);
    put64(p + 812, HEADER); put64(p + 820, (uint64_t)count * RECORD);
    /* Padding/payload after metadata must never be used to finish a record. */
    put64(p + 796, HEADER + CAPACITY * RECORD); put64(p + 804, 16);
    for (unsigned i = 0; i < count && i < CAPACITY; ++i) {
        put32(p + HEADER + i * RECORD, 0x78563412U + i);
        memcpy(p + HEADER + i * RECORD + 4, "permission", 11);
        memcpy(p + HEADER + i * RECORD + 36, "reason", 7);
    }
}
static void reject(const uint8_t *p, size_t size, int capacity)
{
    sfk_perm_request_t out[CAPACITY], before[CAPACITY];
    memset(out, 0xa5, sizeof(out)); memcpy(before, out, sizeof(out));
    if (sfk_get_perm_requests(p, size, out, capacity) >= 0) {
        fprintf(stderr, "accepted malformed SFK permissions (check %u)\n", checks);
        assert(0);
    }
    assert(memcmp(out, before, sizeof(out)) == 0);
    ++checks;
}

int main(void)
{
    uint8_t storage[PACKAGE + 1], *p = storage + 1; /* unaligned input */
    sfk_perm_request_t out[CAPACITY], before[CAPACITY];
    /* Baseline reads past meta_size into otherwise valid file padding. */
    fixture(p, 1); put64(p + 820, RECORD - 1); reject(p, PACKAGE, CAPACITY);
    for (unsigned count = 1; count <= 2; ++count) {
        for (unsigned length = 0; length < count * RECORD; ++length) {
            fixture(p, count); put64(p + 820, length); reject(p, PACKAGE, CAPACITY);
        }
    }
    fixture(p, 1); put64(p + 812, 0); put64(p + 820, 0); reject(p, PACKAGE, CAPACITY);
    fixture(p, 1); put32(p + 776, UINT32_MAX); reject(p, PACKAGE, CAPACITY);
    fixture(p, 1); put32(p + 776, CAPACITY + 1); reject(p, PACKAGE, CAPACITY);
    fixture(p, 1); reject(p, PACKAGE, -1);
    assert(sfk_get_perm_requests(p, PACKAGE, NULL, 1) < 0);
    assert(sfk_get_perm_requests(NULL, PACKAGE, out, 1) < 0);
    reject(p, HEADER - 1, 1);

    for (unsigned count = 0; count <= CAPACITY; ++count) {
        fixture(p, count);
        for (int capacity = 0; capacity <= CAPACITY; ++capacity) {
            memset(out, 0xa5, sizeof(out)); memcpy(before, out, sizeof(out));
            unsigned expected = count < (unsigned)capacity ? count : (unsigned)capacity;
            assert(sfk_get_perm_requests(p, PACKAGE, out, capacity) == (int)expected);
            for (unsigned i = 0; i < expected; ++i) {
                assert(out[i].perm_flag == 0x78563412U + i);
                assert(strcmp(out[i].perm_name, "permission") == 0);
                assert(strcmp(out[i].perm_desc, "reason") == 0);
            }
            assert(memcmp(out + expected, before + expected,
                          sizeof(out) - expected * sizeof(*out)) == 0);
            ++checks;
        }
        assert(sfk_get_perm_requests(p, PACKAGE, NULL, 0) == 0);
    }
    /* A bad later record must reject even a short prefix or validation-only call. */
    for (unsigned field = 0; field < 2; ++field) {
        fixture(p, 2);
        unsigned offset = field ? 36 : 4, length = field ? 128 : 32;
        memset(p + HEADER + RECORD + offset, 'x', length);
        reject(p, PACKAGE, 1); reject(p, PACKAGE, 0);
        assert(sfk_get_perm_requests(p, PACKAGE, NULL, 0) < 0);
        p[HEADER + RECORD + offset + length - 1] = 0;
        assert(sfk_get_perm_requests(p, PACKAGE, out, CAPACITY) == 2);
    }
    fixture(p, 0); put64(p + 812, 0); put64(p + 820, 0);
    assert(sfk_get_perm_requests(p, PACKAGE, NULL, 0) == 0);
    /* An exact file allocation validates EOF boundaries, with no trailing padding. */
    uint8_t exact[HEADER + RECORD];
    fixture(p, 1); memcpy(exact, p, sizeof(exact));
    memset(exact + 796, 0, 16);
    assert(sfk_get_perm_requests(exact, sizeof(exact), out, CAPACITY) == 1);
    /* The copy can overlap the input; flags decode from the moved wire bytes. */
    uint32_t aligned[(PACKAGE + 3) / 4];
    uint8_t *alias = (uint8_t *)aligned;
    fixture(alias, 2);
    sfk_perm_request_t *moved = (sfk_perm_request_t *)(alias + HEADER + 4);
    assert(sfk_get_perm_requests(alias, PACKAGE, moved, 2) == 2);
    assert(moved[0].perm_flag == 0x78563412 && moved[1].perm_flag == 0x78563413);
    assert(strcmp(moved[1].perm_desc, "reason") == 0);
    printf("SFK permission tests passed: %u cases, metadata bounds, quota, strings, atomic output\n", checks);
    return 0;
}
