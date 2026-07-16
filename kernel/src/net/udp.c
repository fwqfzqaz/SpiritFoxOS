/*
 * udp.c - UDP protocol for SpiritFoxOS network stack
 *
 * Implements UDP checksum computation, packet transmission,
 * and reception with socket demultiplexing.
 */

#include "net_udp.h"
#include "net_eth.h"
#include "net_utils.h"
#include "net.h"
#include "net_socket.h"
#include "net_ip.h"
#include "string.h"
#include "vga.h"
#include "hal.h"

uint16_t compute_udp_checksum(ipv4_addr_t src_ip, ipv4_addr_t dst_ip,
                               const void *udp_seg, uint16_t udp_len)
{
    const uint16_t *ptr = (const uint16_t *)udp_seg;
    uint32_t sum = 0;
    uint16_t src_hi = (uint16_t)(src_ip >> 16);
    uint16_t src_lo = (uint16_t)(src_ip & 0xFFFF);
    uint16_t dst_hi = (uint16_t)(dst_ip >> 16);
    uint16_t dst_lo = (uint16_t)(dst_ip & 0xFFFF);

    sum += src_hi; sum += src_lo; sum += dst_hi; sum += dst_lo;
    sum += htons((uint16_t)IP_PROTO_UDP);
    sum += htons(udp_len);

    for (int i = 0; i < udp_len / 2; i++) sum += ptr[i];
    if (udp_len & 1) {
        const uint8_t *byte_ptr = (const uint8_t *)udp_seg;
        sum += (uint16_t)byte_ptr[udp_len - 1] << 8;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

void net_udp_send_packet(ipv4_addr_t src_ip_ho, port_t src_port_ho,
                         ipv4_addr_t dst_ip_ho, port_t dst_port_ho,
                         const void *data, size_t data_len)
{
    uint8_t packet[NET_TX_BUF_SIZE];
    ipv4_header_t *ip = (ipv4_header_t *)packet;
    udp_header_t *udp = (udp_header_t *)(packet + sizeof(ipv4_header_t));

    ipv4_addr_t src_ip_n = htonl(src_ip_ho ? src_ip_ho : INADDR_LOOPBACK);
    ipv4_addr_t dst_ip_n = htonl(dst_ip_ho);

    ip->version_ihl   = 0x45;
    ip->dscp_ecn      = 0;
    ip->total_length  = htons(sizeof(ipv4_header_t) + sizeof(udp_header_t) + data_len);
    ip->identification = htons((uint16_t)(net_random() & 0xFFFF));
    ip->flags_fragment = htons(0x4000);
    ip->ttl           = 64;
    ip->protocol      = IP_PROTO_UDP;
    ip->header_checksum = 0;
    ip->src_ip        = src_ip_n;
    ip->dst_ip        = dst_ip_n;
    ip->header_checksum = compute_ip_checksum(ip);

    udp->src_port = htons(src_port_ho);
    udp->dst_port = htons(dst_port_ho);
    udp->length   = htons(sizeof(udp_header_t) + data_len);
    udp->checksum = 0;

    if (data && data_len > 0)
        memcpy((uint8_t *)(udp + 1), data, data_len);

    uint16_t udp_total_len = sizeof(udp_header_t) + data_len;
    udp->checksum = compute_udp_checksum(ntohl(src_ip_n), ntohl(dst_ip_n), udp, udp_total_len);

    size_t total_len = sizeof(ipv4_header_t) + sizeof(udp_header_t) + data_len;
    net_send_eth(packet, total_len, dst_ip_n);
}

void net_udp_rx(const ipv4_header_t *ip, const void *udp_data, size_t ip_payload_len)
{
    if (ip_payload_len < sizeof(udp_header_t)) return;
    const udp_header_t *udp = (const udp_header_t *)udp_data;

    port_t dst_port_ho = ntohs(udp->dst_port);
    port_t src_port_ho = ntohs(udp->src_port);
    ipv4_addr_t dst_ip_ho = ntohl(ip->dst_ip);
    ipv4_addr_t src_ip_ho = ntohl(ip->src_ip);

    uint16_t udp_len = ntohs(udp->length);
    size_t payload_len = udp_len - sizeof(udp_header_t);
    const void *payload = (const uint8_t *)udp + sizeof(udp_header_t);

    net_socket_t *s = net_find_udp_socket(dst_port_ho, dst_ip_ho);
    if (!s) return;

    uint8_t meta[6];
    memcpy(meta, &src_ip_ho, 4);
    memcpy(meta + 4, &src_port_ho, 2);

    uint64_t iflags = hal_save_interrupts();
    net_rx_buffer_write(s, meta, 6);
    net_rx_buffer_write(s, payload, payload_len);
    hal_restore_interrupts(iflags);
}
