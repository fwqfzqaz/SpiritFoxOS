#include "net_utils.h"
#include "timer.h"

uint32_t net_random(void)
{
    uint64_t t = timer_get_ms();
    t = ((t * 1103515245ULL + 12345ULL) >> 16) & 0x7FFFFFFF;
    return (uint32_t)t;
}

uint16_t compute_checksum(const void *data, size_t len)
{
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;
    size_t i;
    for (i = 0; i < len / 2; i++) sum += ptr[i];
    if (len & 1) {
        const uint8_t *byte_ptr = (const uint8_t *)data;
        sum += byte_ptr[len - 1];   /* 奇数尾部字节作为低 8 位 */
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}
