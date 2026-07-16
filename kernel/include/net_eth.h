#ifndef NET_ETH_H
#define NET_ETH_H
#include "net.h"
void net_send_eth(const void *ip_packet, size_t ip_len, uint32_t dst_ip);
void net_loopback_tx(const void *data, size_t len);
#endif
