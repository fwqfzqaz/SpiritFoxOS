#ifndef SF_BLOCK_CLIENT_H
#define SF_BLOCK_CLIENT_H
#include <stdint.h>

/* Kernel-only client, not a module ABI extension. Pin one sealed generation. */
typedef struct {
    uint64_t endpoint, sector_count;
    uint32_t index, sector_size;
} sf_block_client_t;
#define SF_BLOCK_CLIENT_MAX_BYTES 65536u
int sf_block_client_open(uint32_t index, sf_block_client_t *out);
/* At most 64 KiB, split into 4 KiB messages. No retries and no raw fallback.
 * On failure the entire validated output extent is zero, never a partial read.
 * Structurally invalid output extents are untouched. Check the return code. */
int sf_block_client_read(const sf_block_client_t *, uint64_t lba, uint32_t count,
                         void *data, uint32_t capacity);
#endif
