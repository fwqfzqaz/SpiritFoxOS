#ifndef NET_ICMP_H
#define NET_ICMP_H
#include "net.h"
void net_icmp_rx(const void *data, size_t len, uint32_t src_ip);
int  net_icmp_send_echo(uint32_t dst_ip, uint16_t seq, uint16_t id, const void *data, size_t data_len);
/* Called periodically to check for ping timeouts */
void net_icmp_tick(void);
#endif
