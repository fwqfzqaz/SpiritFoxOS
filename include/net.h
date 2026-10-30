#ifndef SF_NET_H
#define SF_NET_H
#include "sf.h"
#define SF_NET_ARP 0x0806u
#define SF_NET_IPV4 0x0800u
#define SF_NET_ICMP 1u
#define SF_NET_TCP 6u
#define SF_NET_UDP 17u
#define SF_NET_ICMP_ID 0x5346u
#define SF_NET_FRAME_MAX 1536u
#define SF_NET_UDP_PORT 40001u
#define SF_NET_TCP_PORT 40002u
#define SF_NET_PAYLOAD_MAX 248u
#define SF_NET_TCP_FIN 0x01u
#define SF_NET_TCP_SYN 0x02u
#define SF_NET_TCP_RST 0x04u
#define SF_NET_TCP_PSH 0x08u
#define SF_NET_TCP_ACK 0x10u
#define SF_NET_TCP_WINDOW 4096u
#define SF_NET_IPV4_LEN 0x0a00020full
#define SF_NET_GATEWAY 0x0a000202u
#define SF_NET_NETMASK 0xffffff00u
/* Reported by the network module; no pointers or device state cross the ABI. */
typedef struct {
 uint8_t mac[6]; uint16_t reserved;
 uint32_t ip,gateway,netmask;
 uint32_t rx_frames,tx_frames,link;
} SfNetInfo;
_Static_assert(sizeof(SfNetInfo)==32,"network info ABI");
/* One bounded datagram or one-shot TCP exchange; size is limited by the IPC message. */
typedef struct {
 uint32_t ip; uint16_t port; uint16_t bytes; uint8_t data[SF_NET_PAYLOAD_MAX];
} SfNetPacket;
_Static_assert(sizeof(SfNetPacket)==SF_MSG_BYTES,"net packet ABI");
/* Pure framing helpers, shared by the ring-3 driver and host boundary tests. */
uint16_t sf_net_checksum(uint32_t sum,const void*data,unsigned bytes);
int sf_net_parse_ipv4(const char*text,uint32_t*out);
void sf_net_format_ipv4(uint32_t ip,char out[16]);
unsigned sf_net_build_arp_request(uint8_t frame[42],const uint8_t mac[6],uint32_t src,uint32_t dst);
unsigned sf_net_build_icmp_echo(uint8_t frame[58],const uint8_t src_mac[6],const uint8_t dst_mac[6],uint32_t src,uint32_t dst,uint16_t seq);
int sf_net_parse_arp_reply(const uint8_t*frame,unsigned bytes,uint32_t expect_ip,uint8_t mac_out[6]);
int sf_net_parse_icmp_reply(const uint8_t*frame,unsigned bytes,uint32_t expect_src,uint16_t seq);
uint16_t sf_net_udp_checksum(uint32_t src,uint32_t dst,const void*segment,unsigned bytes);
uint16_t sf_net_tcp_checksum(uint32_t src,uint32_t dst,const void*segment,unsigned bytes);
unsigned sf_net_build_udp(uint8_t frame[SF_NET_FRAME_MAX],const uint8_t src_mac[6],const uint8_t dst_mac[6],uint32_t src,uint16_t sport,uint32_t dst,uint16_t dport,const void*payload,unsigned bytes);
int sf_net_parse_udp(const uint8_t*frame,unsigned bytes,uint32_t expect_src,uint16_t expect_sport,uint16_t expect_dport,uint8_t payload[SF_NET_PAYLOAD_MAX],unsigned*payload_bytes);
unsigned sf_net_build_tcp(uint8_t frame[SF_NET_FRAME_MAX],const uint8_t src_mac[6],const uint8_t dst_mac[6],uint32_t src,uint16_t sport,uint32_t dst,uint16_t dport,uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void*payload,unsigned bytes);
int sf_net_parse_tcp(const uint8_t*frame,unsigned bytes,uint32_t expect_src,uint16_t expect_sport,uint16_t expect_dport,uint32_t*seq,uint32_t*ack,uint8_t*flags,uint16_t*window,uint8_t payload[SF_NET_PAYLOAD_MAX],unsigned*payload_bytes);
#endif
