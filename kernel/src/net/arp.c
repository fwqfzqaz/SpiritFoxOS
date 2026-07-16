/*
 * arp.c - ARP protocol for SpiritFoxOS network stack
 *
 * Implements ARP cache management, request/reply generation, and
 * IP-to-MAC resolution with polling for the SpiritFoxOS kernel.
 */

#include "net_arp.h"
#include "net_utils.h"
#include "net.h"
#include "rtl8139.h"
#include "kmalloc.h"
#include "string.h"
#include "vga.h"
#include "timer.h"
#include "hal.h"

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    uint64_t expire;
} arp_entry_t;

static arp_entry_t arp_cache[16];

void net_arp_init(void) { memset(arp_cache, 0, sizeof(arp_cache)); }

static void arp_cache_insert(uint32_t ip, const uint8_t mac[6])
{
    uint64_t now = timer_get_ms();
    for (int i = 0; i < 16; i++) {
        if (arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].expire = now + 60000;
            return;
        }
    }
    for (int i = 0; i < 16; i++) {
        if (arp_cache[i].ip == 0 || arp_cache[i].expire <= now) {
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].expire = now + 60000;
            return;
        }
    }
    arp_cache[0].ip = ip;
    memcpy(arp_cache[0].mac, mac, 6);
    arp_cache[0].expire = now + 60000;
}

static int arp_cache_lookup(uint32_t ip, uint8_t *mac_out)
{
    uint64_t now = timer_get_ms();
    for (int i = 0; i < 16; i++) {
        if (arp_cache[i].ip == ip && arp_cache[i].expire > now) {
            memcpy(mac_out, arp_cache[i].mac, 6);
            return 0;
        }
    }
    return -1;
}

static void arp_send_request(uint32_t target_ip)
{
    uint8_t frame[14 + sizeof(arp_packet_t)];
    eth_header_t *eth = (eth_header_t *)frame;
    arp_packet_t *arp = (arp_packet_t *)(frame + 14);

    memset(eth->dst_mac, 0xFF, 6);
    memcpy(eth->src_mac, net_local_mac, 6);
    eth->ethertype = htons(0x0806);

    arp->htype = htons(1);
    arp->ptype = htons(0x0800);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(1);
    memcpy(arp->sha, net_local_mac, 6);
    arp->spa   = net_local_ip;
    memset(arp->tha, 0x00, 6);
    arp->tpa   = target_ip;

    rtl8139_send(frame, sizeof(frame));
}

static void arp_send_reply(uint32_t target_ip, const uint8_t target_mac[6])
{
    uint8_t frame[14 + sizeof(arp_packet_t)];
    eth_header_t *eth = (eth_header_t *)frame;
    arp_packet_t *arp = (arp_packet_t *)(frame + 14);

    memcpy(eth->dst_mac, target_mac, 6);
    memcpy(eth->src_mac, net_local_mac, 6);
    eth->ethertype = htons(0x0806);

    arp->htype = htons(1);
    arp->ptype = htons(0x0800);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(2);
    memcpy(arp->sha, net_local_mac, 6);
    arp->spa   = net_local_ip;
    memcpy(arp->tha, target_mac, 6);
    arp->tpa   = target_ip;

    rtl8139_send(frame, sizeof(frame));
}

void net_arp_rx(const void *data, size_t len)
{
    if (len < sizeof(arp_packet_t)) return;
    const arp_packet_t *arp = (const arp_packet_t *)data;
    if (ntohs(arp->htype) != 1 || ntohs(arp->ptype) != 0x0800) return;
    if (arp->hlen != 6 || arp->plen != 4) return;

    uint16_t oper = ntohs(arp->oper);
    if (oper == 2) {
        if (arp->tpa == net_local_ip) {
            arp_cache_insert(arp->spa, arp->sha);
        }
        return;
    }
    if (oper == 1) {
        if (arp->tpa == net_local_ip) {
            arp_cache_insert(arp->spa, arp->sha);
            arp_send_reply(arp->spa, arp->sha);
        }
    }
}

int net_arp_resolve(uint32_t ip, uint8_t *mac_out)
{
    if (arp_cache_lookup(ip, mac_out) == 0) return 0;
    arp_send_request(ip);
    uint64_t deadline = timer_get_ms() + 1000;
    while (timer_get_ms() < deadline) {
        hal_io_wait();
        if (arp_cache_lookup(ip, mac_out) == 0) return 0;
    }
    return -1;
}
