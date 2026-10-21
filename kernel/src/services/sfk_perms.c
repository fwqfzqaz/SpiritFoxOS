/* Permissions are the first fixed-width records in the SFK v1 metadata. */
#include "pkgmgr.h"
#include "string.h"

_Static_assert(sizeof(sfk_perm_request_t) == 164, "SFK v1 permission size");
_Static_assert(offsetof(sfk_perm_request_t, perm_name) == 4, "SFK v1 permission name");
_Static_assert(offsetof(sfk_perm_request_t, perm_desc) == 36, "SFK v1 permission description");

static int terminated(const uint8_t *p, size_t capacity)
{
    for (size_t i = 0; i < capacity; ++i) if (p[i] == 0) return 1;
    return 0;
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int sfk_get_perm_requests(const void *data, size_t size,
                          sfk_perm_request_t *perms, int max_perms)
{
    sfk_header_t hdr;
    if (max_perms < 0 || (max_perms > 0 && !perms) ||
        sfk_parse_header(data, size, &hdr) < 0) return -1;
    if (hdr.nperms > SFK_V1_MAX_PERMS) return -1;
    if (hdr.nperms == 0) return 0;
    if (hdr.meta_offset == 0 ||
        hdr.nperms > hdr.meta_size / sizeof(sfk_perm_request_t)) return -1;

    /* The header bounded meta_offset/meta_size within the input snapshot. */
    const uint8_t *meta = (const uint8_t *)data + hdr.meta_offset;
    for (uint32_t i = 0; i < hdr.nperms; ++i) {
        const uint8_t *record = meta + i * sizeof(sfk_perm_request_t);
        if (!terminated(record + 4, 32) || !terminated(record + 36, 128))
            return -1;
    }

    /* Validate ALL declared records before publishing even a short prefix. */
    uint32_t count = hdr.nperms;
    if (count > (uint32_t)max_perms) count = (uint32_t)max_perms;
    if (count == 0) return 0; /* NULL/0 is validation-only. */
    memmove(perms, meta, count * sizeof(*perms));
    for (uint32_t i = 0; i < count; ++i)
        perms[i].perm_flag = read_le32((const uint8_t *)&perms[i]);
    return (int)count;
}
