#ifndef NET_UDP_H
#define NET_UDP_H
#include "net.h"
void net_udp_rx(const ipv4_header_t *ip, const void *udp_data, size_t ip_payload_len);
void net_udp_send_packet(ipv4_addr_t src_ip_ho, port_t src_port_ho, ipv4_addr_t dst_ip_ho, port_t dst_port_ho, const void *data, size_t data_len);
uint16_t compute_udp_checksum(ipv4_addr_t src_ip, ipv4_addr_t dst_ip, const void *udp_seg, uint16_t udp_len);
#endif
