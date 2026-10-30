#ifndef NET_UTILS_H
#define NET_UTILS_H
#include <stdint.h>
#include <stddef.h>
static inline uint16_t htons(uint16_t v) { return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) { return ((v & 0xFF) << 24) | (((v >> 8) & 0xFF) << 16) | (((v >> 16) & 0xFF) << 8) | ((v >> 24) & 0xFF); }
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }
uint16_t compute_checksum(const void *data, size_t len);
uint32_t net_random(void);
#endif
