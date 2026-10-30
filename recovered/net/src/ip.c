#include "net_ip.h"
#include "net_utils.h"
#include "net_tcp.h"
#include "net_udp.h"
#include "net_icmp.h"
#include "net.h"
#include "string.h"
#include "vga.h"

uint16_t compute_ip_checksum(const ipv4_header_t *hdr)
{
    const uint16_t *ptr = (const uint16_t *)hdr;
    uint32_t sum = 0;
    int len = ((hdr->version_ihl & 0x0F) * 4) / 2;
    for (int i = 0; i < len; i++) sum += ptr[i];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

void net_rx_ipv4(const void *data, size_t len)
{
    if (!data || len < sizeof(ipv4_header_t)) return;
    const ipv4_header_t *ip = (const ipv4_header_t *)data;
    if ((ip->version_ihl >> 4) != 4) return;

    uint16_t total_length = ntohs(ip->total_length);
    if (total_length > len) return;

    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    if (ihl < sizeof(ipv4_header_t)) return;

    const void *payload = (const uint8_t *)data + ihl;
    size_t payload_len = total_length - ihl;

    switch (ip->protocol) {
    case IP_PROTO_TCP:
        net_tcp_rx(ip, payload, payload_len);
        break;
    case IP_PROTO_UDP:
        net_udp_rx(ip, payload, payload_len);
        break;
    case IP_PROTO_ICMP:
        net_icmp_rx(payload, payload_len, ip->src_ip);
        break;
    default:
        break;
    }
}
