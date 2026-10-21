/* Bounded SFK v1 registry records; values borrow the caller's input snapshot. */
#include "pkgmgr.h"
#include "kmalloc.h"
#include "string.h"

_Static_assert(sizeof(sfk_reg_entry_t) == 392, "SFK v1 registry header size");
_Static_assert(offsetof(sfk_reg_entry_t, value_name) == 256, "SFK registry name");
_Static_assert(offsetof(sfk_reg_entry_t, value_type) == 384, "SFK registry type");
_Static_assert(offsetof(sfk_reg_entry_t, value_size) == 388, "SFK registry size");

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int terminated(const char *text, size_t capacity)
{
    for (size_t i = 0; i < capacity; ++i) if (!text[i]) return 1;
    return 0;
}

static int scan(const void *data, size_t size, sfk_reg_view_t *views)
{
    sfk_header_t hdr;
    if (sfk_parse_header(data, size, &hdr) < 0 ||
        hdr.nreg_entries > SFK_V1_MAX_REG_ENTRIES ||
        sfk_get_perm_requests(data, size, NULL, 0) < 0) return -1;
    if (hdr.nreg_entries == 0) return 0;
    if (hdr.meta_offset == 0) return -1;
    const uint8_t *meta = (const uint8_t *)data + hdr.meta_offset;
    uint64_t offset = (uint64_t)hdr.nperms * sizeof(sfk_perm_request_t);
    for (uint32_t i = 0; i < hdr.nreg_entries; ++i) {
        if (offset > hdr.meta_size || hdr.meta_size - offset < sizeof(sfk_reg_entry_t))
            return -1;
        const uint8_t *record = meta + offset;
        sfk_reg_entry_t entry;
        memcpy(&entry, record, sizeof(entry));
        entry.value_type = read_le32(record + 384);
        entry.value_size = read_le32(record + 388);
        if (!entry.key_path[0] || !terminated(entry.key_path, sizeof(entry.key_path)) ||
            !terminated(entry.value_name, sizeof(entry.value_name)) ||
            entry.value_size > REG_MAX_VALUE_DATA ||
            entry.value_size > hdr.meta_size - offset - sizeof(entry)) return -1;
        if (views) {
            views[i].entry = entry;
            views[i].value_data = record + sizeof(entry);
        }
        offset += sizeof(entry) + entry.value_size;
    }
    return (int)hdr.nreg_entries;
}

int sfk_get_reg_views(const void *data, size_t size,
                      sfk_reg_view_t **entries, int *count)
{
    if (!entries || !count) return -1;
    int total = scan(data, size, NULL);
    if (total < 0) return -1;
    sfk_reg_view_t *result = NULL;
    if (total) {
        result = kcalloc((size_t)total, sizeof(*result));
        if (!result) return -1;
        if (scan(data, size, result) != total) {
            kfree(result);
            return -1;
        }
    }
    *entries = result;
    *count = total;
    return 0;
}

/* Compatibility API: a headers-only owned array, never inline value storage. */
int sfk_get_reg_entries(const void *data, size_t size,
                        sfk_reg_entry_t **entries, int *count)
{
    if (!entries || !count) return -1;
    sfk_reg_view_t *views = NULL;
    int total;
    if (sfk_get_reg_views(data, size, &views, &total) < 0) return -1;
    sfk_reg_entry_t *result = NULL;
    if (total) {
        result = kcalloc((size_t)total, sizeof(*result));
        if (!result) { kfree(views); return -1; }
        for (int i = 0; i < total; ++i) result[i] = views[i].entry;
    }
    kfree(views);
    *entries = result;
    *count = total;
    return 0;
}
