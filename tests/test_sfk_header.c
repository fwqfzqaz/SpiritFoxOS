/* Fixed wire offsets deliberately do not use offsetof() or struct fixtures. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pkgmgr.h"

enum { HEADER = 1084, PACKAGE = HEADER + 64 };
static unsigned checks;

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static void put64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static void fixture(uint8_t *p)
{
    memset(p, 0, PACKAGE);
    /* SFK_MAGIC=0x53464b21 on the wire, not the comment's "SFK!". */
    p[0] = 0x21; p[1] = 0x4b; p[2] = 0x46; p[3] = 0x53;
    p[4] = 1;
    memcpy(p + 8, "org.spiritfox.hello-1_test", sizeof("org.spiritfox.hello-1_test"));
    memcpy(p + 72, "Hello", 6);
    memcpy(p + 136, "1.0", 4);
    memcpy(p + 232, "UTF-8: \xe7\x81\xb5\xe7\x8b\x90", 14);
    put64(p + 796, HEADER);
    put64(p + 804, 32);
    put64(p + 812, HEADER + 32);
    put64(p + 820, 32);
    memset(p + 828, 0xa5, 256); /* opaque, not authenticated */
}

static void reject(const void *p, size_t size)
{
    sfk_header_t out, before;
    memset(&out, 0x5a, sizeof(out));
    memcpy(&before, &out, sizeof(out));
    if (sfk_parse_header(p, size, &out) >= 0) {
        fprintf(stderr, "accepted malformed SFK header (check %u)\n", checks);
        abort();
    }
    assert(memcmp(&out, &before, sizeof(out)) == 0);
    ++checks;
}

static sfk_header_t accept(const void *p, size_t size)
{
    sfk_header_t out;
    assert(sfk_parse_header(p, size, &out) == 0);
    ++checks;
    return out;
}

int main(void)
{
    uint8_t bytes[PACKAGE + 1], *p = bytes + 1; /* deliberately unaligned */
    fixture(p);
    sfk_header_t out = accept(p, PACKAGE);
    assert(sizeof(out) == HEADER && out.magic == SFK_MAGIC && out.version == 1);
    assert(out.data_offset == HEADER && out.data_size == 32);
    assert(out.meta_offset == HEADER + 32 && out.meta_size == 32);
    assert(out.signature[0] == 0xa5 && out.signature[255] == 0xa5);
    assert(strcmp(out.pkg_id, "org.spiritfox.hello-1_test") == 0);
    /* Baseline incorrectly accepts version zero; keep this first regression. */
    p[4] = 0; reject(p, PACKAGE);
    p[4] = 2; reject(p, PACKAGE);
    fixture(p); memcpy(p, "SFK!", 4); reject(p, PACKAGE);
    fixture(p); reject(NULL, PACKAGE);
    assert(sfk_parse_header(p, PACKAGE, NULL) < 0);
    for (size_t size = 0; size < HEADER; ++size) reject(p, size);
    /* An exactly-sized allocation catches accidental reads beyond the header. */
    uint8_t *minimal = malloc(HEADER);
    assert(minimal);
    memcpy(minimal, p, HEADER);
    memset(minimal + 796, 0, 32);
    accept(minimal, HEADER);
    free(minimal);

    const unsigned string_offsets[] = {8, 72, 136, 168, 232, 488, 520, 648};
    const unsigned capacities[] = {64, 64, 32, 64, 256, 32, 128, 128};
    for (size_t i = 0; i < 8; ++i) {
        fixture(p); memset(p + string_offsets[i], 'x', capacities[i]);
        reject(p, PACKAGE);
        p[string_offsets[i] + capacities[i] - 1] = 0;
        accept(p, PACKAGE);
    }
    const char *bad_ids[] = {"", ".", "..", "../escape", "a/b", "a\\b",
                             "/root", "a b", "a\n", "\xff"};
    for (size_t i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); ++i) {
        fixture(p); memset(p + 8, 0, 64);
        memcpy(p + 8, bad_ids[i], strlen(bad_ids[i])); reject(p, PACKAGE);
    }
    /* Both section descriptors: header overlap, EOF, and 64-bit overflow. */
    for (unsigned field = 796; field <= 812; field += 16) {
        fixture(p); put64(p + field, HEADER - 1); reject(p, PACKAGE);
        fixture(p); put64(p + field, 0); reject(p, PACKAGE);
        fixture(p); put64(p + field, PACKAGE + 1); reject(p, PACKAGE);
        fixture(p); put64(p + field, UINT64_MAX); reject(p, PACKAGE);
        fixture(p); put64(p + field + 8, UINT64_MAX); reject(p, PACKAGE);
        fixture(p); put64(p + field, PACKAGE); reject(p, PACKAGE);
        fixture(p); put64(p + field + 8, PACKAGE); reject(p, PACKAGE);
        fixture(p); put64(p + field, 0); put64(p + field + 8, 0);
        accept(p, PACKAGE);
        fixture(p); put64(p + field, PACKAGE); put64(p + field + 8, 0);
        accept(p, PACKAGE);
        fixture(p); put64(p + field, HEADER - 1); put64(p + field + 8, 0);
        reject(p, PACKAGE);
    }
    fixture(p); put64(p + 812, HEADER + 31); reject(p, PACKAGE);
    fixture(p); put64(p + 812, HEADER); reject(p, PACKAGE);
    fixture(p); put64(p + 796, HEADER + 32); put64(p + 812, HEADER);
    accept(p, PACKAGE); /* reversed adjacent sections */
    put64(p + 820, 33); reject(p, PACKAGE);
    fixture(p); put64(p + 804, 31); accept(p, PACKAGE); /* padding */
    /* Counts are decoded, not validated as metadata records by this API. */
    fixture(p);
    for (unsigned i = 0; i < 5; ++i) put32(p + 776 + 4 * i, 0x10203040 + i);
    out = accept(p, PACKAGE);
    assert(out.nperms == 0x10203040 && out.nreg_entries == 0x10203041);
    assert(out.nfile_assocs == 0x10203042 && out.nservices == 0x10203043);
    assert(out.ndependencies == 0x10203044);
    printf("SFK v1 header tests passed: %u cases, fixed wire layout, bounds, strings, identity\n", checks);
    return 0;
}
