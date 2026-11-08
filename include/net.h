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
#define SF_NET_DHCP_DISCOVER 1u
#define SF_NET_DHCP_OFFER 2u
#define SF_NET_DHCP_REQUEST 3u
#define SF_NET_DHCP_ACK 5u
#define SF_NET_DHCP_CLIENT_PORT 68u
#define SF_NET_DHCP_SERVER_PORT 67u
/* 网络信息（由网络模块报告；ABI 不跨界传递指针或设备状态）。 */
typedef struct {
 uint8_t mac[6]; uint16_t reserved;
 uint32_t ip,gateway,netmask;
 uint32_t rx_frames,tx_frames,link;
} SfNetInfo;
_Static_assert(sizeof(SfNetInfo)==32,"network info ABI");
/* 单条有界数据报或一次性 TCP 交换；尺寸受 IPC 消息上限约束。 */
typedef struct {
 uint32_t ip; uint16_t port; uint16_t bytes; uint8_t data[SF_NET_PAYLOAD_MAX];
} SfNetPacket;
_Static_assert(sizeof(SfNetPacket)==SF_MSG_BYTES,"net packet ABI");
/* 纯帧构造/解析辅助（Rust 实现：rust/sfcore/src/net.rs，符号同名）。
 * ring-3 驱动与宿主边界测试共用；全部多字节整数按网络序（大端）。 */
/* Internet 校验和：以 sum 为初值累加大端 16 位字后折叠取反。 */
uint16_t sf_net_checksum(uint32_t sum,const void*data,unsigned bytes);
/* 解析 a.b.c.d 点分地址；每段 0–255，返回 SF_OK/SF_INVALID。 */
int sf_net_parse_ipv4(const char*text,uint32_t*out);
/* 格式化点分地址到 16 字节缓冲（含 NUL）。 */
void sf_net_format_ipv4(uint32_t ip,char out[16]);
/* 构造 42 字节 ARP 请求帧（广播 MAC），返回帧长。 */
unsigned sf_net_build_arp_request(uint8_t frame[42],const uint8_t mac[6],uint32_t src,uint32_t dst);
/* 构造 58 字节 ICMP 回显请求帧，返回帧长。 */
unsigned sf_net_build_icmp_echo(uint8_t frame[58],const uint8_t src_mac[6],const uint8_t dst_mac[6],uint32_t src,uint32_t dst,uint16_t seq);
/* 解析 ARP 应答：命中 expect_ip 时导出对方 MAC，返回 1。 */
int sf_net_parse_arp_reply(const uint8_t*frame,unsigned bytes,uint32_t expect_ip,uint8_t mac_out[6]);
/* 解析 ICMP 回显应答（校验 IPv4 头校验和/协议/标识/序号），返回 1。 */
int sf_net_parse_icmp_reply(const uint8_t*frame,unsigned bytes,uint32_t expect_src,uint16_t seq);
/* UDP/TCP 伪头校验和（协议号分别固定为 17/6）。 */
uint16_t sf_net_udp_checksum(uint32_t src,uint32_t dst,const void*segment,unsigned bytes);
uint16_t sf_net_tcp_checksum(uint32_t src,uint32_t dst,const void*segment,unsigned bytes);
/* 构造 UDP 数据报帧（载荷 ≤ SF_NET_PAYLOAD_MAX），返回帧长；越界返回 0。 */
unsigned sf_net_build_udp(uint8_t frame[SF_NET_FRAME_MAX],const uint8_t src_mac[6],const uint8_t dst_mac[6],uint32_t src,uint16_t sport,uint32_t dst,uint16_t dport,const void*payload,unsigned bytes);
/* 解析 UDP 帧：校验头校验和/端口/长度，导出载荷 ≤248 字节，返回 1。 */
int sf_net_parse_udp(const uint8_t*frame,unsigned bytes,uint32_t expect_src,uint16_t expect_sport,uint16_t expect_dport,uint8_t payload[SF_NET_PAYLOAD_MAX],unsigned*payload_bytes);
/* 构造一次性 TCP 段帧（IHL 5、数据偏移 5），返回帧长；越界返回 0。 */
unsigned sf_net_build_tcp(uint8_t frame[SF_NET_FRAME_MAX],const uint8_t src_mac[6],const uint8_t dst_mac[6],uint32_t src,uint16_t sport,uint32_t dst,uint16_t dport,uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void*payload,unsigned bytes);
/* 解析 TCP 段：强校验和，导出 seq/ack/flags/window 与载荷，返回 1。 */
int sf_net_parse_tcp(const uint8_t*frame,unsigned bytes,uint32_t expect_src,uint16_t expect_sport,uint16_t expect_dport,uint32_t*seq,uint32_t*ack,uint8_t*flags,uint16_t*window,uint8_t payload[SF_NET_PAYLOAD_MAX],unsigned*payload_bytes);
/* 构造 DHCP 客户端帧（DISCOVER/REQUEST，无需预先配置地址），返回帧长。 */
unsigned sf_net_build_dhcp(uint8_t frame[SF_NET_FRAME_MAX],const uint8_t mac[6],uint32_t xid,uint8_t msg_type,uint32_t requested_ip,uint32_t server_ip);
/* 解析 DHCP 服务端应答：命中 xid 与消息类型时导出地址/网关/掩码/服务端，返回 1。 */
int sf_net_parse_dhcp(const uint8_t*frame,unsigned bytes,uint32_t xid,uint8_t expect_type,uint32_t*ip_out,uint32_t*gateway_out,uint32_t*netmask_out,uint32_t*server_out);
#define SF_NET_CONN_MAX 8u
#define SF_NET_CONN_TCP 6u
#define SF_NET_CONN_OPEN 1u
#define SF_NET_CONN_ESTABLISHED 2u
#define SF_NET_CONN_CLOSED 3u
#define SF_NET_CONN_PAYLOAD_MAX 240u
/* 有界连接对象：句柄由服务端分配，客户端只持有不透明 u32。 */
typedef struct {uint32_t peer_ip;uint16_t peer_port;uint8_t protocol;uint8_t reserved;} SfNetConnRequest;
typedef struct {uint32_t handle,peer_ip,peer_port,local_port,bytes_sent,bytes_received;uint8_t state,protocol;uint8_t reserved[2];} SfNetConnInfo;
typedef struct {uint32_t handle,bytes;uint8_t payload[SF_NET_CONN_PAYLOAD_MAX];} SfNetConnSend;
typedef struct {uint32_t handle;uint16_t timeout_ticks;uint16_t reserved;} SfNetConnRecv;
#endif
