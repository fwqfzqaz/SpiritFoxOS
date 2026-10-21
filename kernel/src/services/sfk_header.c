/* SFK v1 header envelope only; metadata records need separate validation. */
#include "pkgmgr.h"
#include "string.h"

_Static_assert(sizeof(sfk_header_t) == SFK_V1_HEADER_SIZE, "SFK v1 header size");
_Static_assert(offsetof(sfk_header_t, nperms) == 776, "SFK v1 count offset");
_Static_assert(offsetof(sfk_header_t, data_offset) == 796, "SFK v1 data offset");
_Static_assert(offsetof(sfk_header_t, meta_offset) == 812, "SFK v1 metadata offset");
_Static_assert(offsetof(sfk_header_t, signature) == 828, "SFK v1 signature offset");

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t read_le64(const uint8_t *p)
{
    return (uint64_t)read_le32(p) | (uint64_t)read_le32(p + 4) << 32;
}

static int terminated(const char *text, size_t capacity)
{
    for (size_t i = 0; i < capacity; ++i) {
        if (text[i] == '\0') return 1;
    }
    return 0;
}

/* pkg_id becomes one component of /opt/sfk/<id> and a registry path. */
static int valid_id(const char *id)
{
    if (!id[0] || strcmp(id, ".") == 0 || strcmp(id, "..") == 0) return 0;
    for (size_t i = 0; id[i]; ++i) {
        unsigned char c = (unsigned char)id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static int valid_section(uint64_t offset, uint64_t length, size_t size)
{
    if (offset == 0 && length == 0) return 1; /* absent */
    return offset >= SFK_V1_HEADER_SIZE && offset <= size &&
           length <= size - offset;
}

int sfk_parse_header(const void *data, size_t size, sfk_header_t *header)
{
    sfk_header_t candidate;
    if (!data || !header || size < SFK_V1_HEADER_SIZE) return -1;
    const uint8_t *bytes = data;
    memcpy(&candidate, bytes, sizeof(candidate));

    /* Do not depend on host endianness or aligned input. */
    candidate.magic = read_le32(bytes);
    candidate.version = read_le32(bytes + 4);
    candidate.nperms = read_le32(bytes + 776);
    candidate.nreg_entries = read_le32(bytes + 780);
    candidate.nfile_assocs = read_le32(bytes + 784);
    candidate.nservices = read_le32(bytes + 788);
    candidate.ndependencies = read_le32(bytes + 792);
    candidate.data_offset = read_le64(bytes + 796);
    candidate.data_size = read_le64(bytes + 804);
    candidate.meta_offset = read_le64(bytes + 812);
    candidate.meta_size = read_le64(bytes + 820);

    if (candidate.magic != SFK_MAGIC || candidate.version != SFK_VERSION)
        return -1;
    if (!terminated(candidate.pkg_id, sizeof(candidate.pkg_id)) ||
        !terminated(candidate.pkg_name, sizeof(candidate.pkg_name)) ||
        !terminated(candidate.pkg_version, sizeof(candidate.pkg_version)) ||
        !terminated(candidate.vendor, sizeof(candidate.vendor)) ||
        !terminated(candidate.description, sizeof(candidate.description)) ||
        !terminated(candidate.category, sizeof(candidate.category)) ||
        !terminated(candidate.icon_path, sizeof(candidate.icon_path)) ||
        !terminated(candidate.exec_path, sizeof(candidate.exec_path)) ||
        !valid_id(candidate.pkg_id)) return -1;

    if (!valid_section(candidate.data_offset, candidate.data_size, size) ||
        !valid_section(candidate.meta_offset, candidate.meta_size, size)) return -1;
    if (candidate.data_size && candidate.meta_size) {
        if (candidate.data_offset <= candidate.meta_offset) {
            if (candidate.data_size > candidate.meta_offset - candidate.data_offset)
                return -1;
        } else if (candidate.meta_size > candidate.data_offset - candidate.meta_offset) {
            return -1;
        }
    }
    /* Publish only a fully validated header; leave output unchanged on error. */
    memcpy(header, &candidate, sizeof(candidate));
    return 0;
}
