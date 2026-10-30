#ifndef NET_ARP_H
#define NET_ARP_H
#include "net.h"
void net_arp_init(void);
int  net_arp_resolve(uint32_t ip, uint8_t *mac_out);
void net_arp_rx(const void *data, size_t len);
#endif
