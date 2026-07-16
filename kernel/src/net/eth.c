/*
 * eth.c - Ethernet layer and loopback for SpiritFoxOS network stack
 *
 * Handles wrapping IP packets into Ethernet frames, ARP resolution
 * for next-hop MAC addresses, and loopback injection for local IP
 * traffic.
 */

#include "net_eth.h"
#include "net_arp.h"
#include "net_ip.h"
#include "net_utils.h"
#include "rtl8139.h"
#include "kmalloc.h"
#include "string.h"
#include "vga.h"
#include "timer.h"
#include "hal.h"

void net_send_eth(const void *ip_packet, size_t ip_len, uint32_t dst_ip)
{
    if (!ip_packet || ip_len == 0) return;

    uint8_t first_byte = (ntohl(dst_ip) >> 24) & 0xFF;
    if (first_byte == 127) { net_loopback_tx(ip_packet, ip_len); return; }

    if (!net_hw_initialized) { net_loopback_tx(ip_packet, ip_len); return; }

    uint32_t next_hop_ip;
    if ((dst_ip & net_netmask) == (net_local_ip & net_netmask)) {
        next_hop_ip = dst_ip;
    } else {
        next_hop_ip = net_gateway_ip;
    }

    uint8_t dst_mac[6];
    if (net_arp_resolve(next_hop_ip, dst_mac) != 0) return;

    size_t frame_len = sizeof(eth_header_t) + ip_len;
    uint8_t *frame = (uint8_t *)kmalloc(frame_len);
    if (!frame) return;

    eth_header_t *eth = (eth_header_t *)frame;
    memcpy(eth->dst_mac, dst_mac, 6);
    memcpy(eth->src_mac, net_local_mac, 6);
    eth->ethertype = htons(0x0800);
    memcpy(frame + sizeof(eth_header_t), ip_packet, ip_len);

    rtl8139_send(frame, frame_len);
    kfree(frame);
}

void net_loopback_tx(const void *data, size_t len)
{
    if (!data || len < sizeof(ipv4_header_t)) return;
    const ipv4_header_t *ip = (const ipv4_header_t *)data;
    if ((ip->version_ihl >> 4) != 4) return;
    /* For loopback, deliver directly without strict checksum verification */
    net_rx_ipv4(data, len);
}
