#ifndef NET_TCP_H
#define NET_TCP_H
#include "net.h"
void net_tcp_rx(const ipv4_header_t *ip, const void *tcp_data, size_t ip_payload_len);
void net_tcp_send_packet(net_socket_t *s, uint8_t tcp_flags, const void *data, size_t data_len);
void net_tcp_tick(void);  /* Called periodically for retransmission, TIME_WAIT, etc. */
uint16_t compute_tcp_checksum(ipv4_addr_t src_ip, ipv4_addr_t dst_ip, const void *tcp_seg, uint16_t tcp_len);
#endif
