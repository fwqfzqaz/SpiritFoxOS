#ifndef NET_IP_H
#define NET_IP_H
#include "net.h"
void net_rx_ipv4(const void *data, size_t len);
uint16_t compute_ip_checksum(const ipv4_header_t *hdr);
#endif
