#ifndef GUNZIP_H
#define GUNZIP_H

#include <stdint.h>
#include <stddef.h>

/* Decompress gzip-formatted data (RFC 1952).
 * Allocates output buffer via kmalloc; caller must kfree() it.
 * Returns 0 on success, negative on error. */
int gunzip(const void *src, size_t src_size, void **dst, size_t *dst_size);

/* Decompress raw deflate data (RFC 1951) into a pre-allocated buffer.
 * Returns 0 on success, negative on error. */
int zlib_inflate(const void *src, size_t src_size, void *dst,
                 size_t dst_cap, size_t *dst_size);

/* Streaming gzip decompression callback.
 * Called with each chunk of decompressed output data.
 * Return 0 to continue, negative to abort. */
typedef int (*gunzip_output_cb)(const void *data, size_t len, void *ctx);

/* Stream-decompress gzip data from a VFS file descriptor.
 * Reads compressed data incrementally, calling output_cb for each chunk
 * of decompressed data. Never allocates more than ~4MB at a time.
 *
 * @fd:         VFS file descriptor (must be open for reading)
 * @gz_offset:  offset of the gzip data within the file
 * @gz_size:    size of the gzip data in bytes
 * @output_cb:  callback for decompressed output chunks
 * @ctx:        user context passed to output_cb
 *
 * Returns 0 on success, negative on error. */
int gunzip_stream_vfs(int fd, size_t gz_offset, size_t gz_size,
                      gunzip_output_cb output_cb, void *ctx);

#endif /* GUNZIP_H */
