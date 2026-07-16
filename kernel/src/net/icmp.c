#include "net_icmp.h"
#include "net_ip.h"
#include "net_eth.h"
#include "net_utils.h"
#include "net.h"
#include "kmalloc.h"
#include "string.h"
#include "vga.h"
#include "timer.h"

/* ICMP type codes */
#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_DEST_UNREACH 3
#define ICMP_TYPE_ECHO_REQUEST 8
#define ICMP_TYPE_TIME_EXCEED  11

/* Ping state for tracking outgoing echo requests */
typedef struct {
    uint32_t dst_ip;        /* Network byte order */
    uint16_t id;
    uint16_t seq;
    uint64_t send_time;     /* ms timestamp */
    int      active;
    int      timeout;       /* 1 if timed out */
    uint32_t rtt;           /* Round trip time in ms */
} ping_state_t;

#define MAX_PING_STATES 8
static ping_state_t ping_states[MAX_PING_STATES];

static int find_ping_slot(uint32_t dst_ip, uint16_t id)
{
    for (int i = 0; i < MAX_PING_STATES; i++) {
        if (ping_states[i].active && ping_states[i].dst_ip == dst_ip && ping_states[i].id == id)
            return i;
    }
    return -1;
}

static int alloc_ping_slot(void)
{
    for (int i = 0; i < MAX_PING_STATES; i++) {
        if (!ping_states[i].active) return i;
    }
    return -1;
}

/* Send ICMP echo request */
int net_icmp_send_echo(uint32_t dst_ip, uint16_t seq, uint16_t id, const void *data, size_t data_len)
{
    /* Allocate ping tracking slot */
    int slot = alloc_ping_slot();
    if (slot < 0) return -1;

    size_t icmp_len = sizeof(icmp_header_t) + data_len;
    uint8_t *icmp_buf = (uint8_t *)kmalloc(icmp_len);
    if (!icmp_buf) return -1;

    icmp_header_t *icmp = (icmp_header_t *)icmp_buf;
    icmp->type = ICMP_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    /* Standard ICMP Echo: rest_of_header = identifier(high16) | sequence(low16) */
    icmp->rest_of_header = ((uint32_t)htons(id) << 16) | htons(seq);

    if (data && data_len > 0)
        memcpy(icmp_buf + sizeof(icmp_header_t), data, data_len);

    icmp->checksum = compute_checksum(icmp_buf, icmp_len);

    /* Build IP + ICMP packet */
    size_t total_len = sizeof(ipv4_header_t) + icmp_len;
    uint8_t *packet = (uint8_t *)kmalloc(total_len);
    if (!packet) { kfree(icmp_buf); return -1; }

    ipv4_header_t *ip = (ipv4_header_t *)packet;
    ip->version_ihl   = 0x45;
    ip->dscp_ecn      = 0;
    ip->total_length  = htons(total_len);
    ip->identification = htons((uint16_t)(net_random() & 0xFFFF));
    ip->flags_fragment = htons(0x4000);
    ip->ttl           = 64;
    ip->protocol      = IP_PROTO_ICMP;
    ip->header_checksum = 0;
    ip->src_ip        = net_local_ip;
    ip->dst_ip        = dst_ip;
    ip->header_checksum = compute_ip_checksum(ip);

    memcpy(packet + sizeof(ipv4_header_t), icmp_buf, icmp_len);
    kfree(icmp_buf);

    /* Track the ping */
    ping_states[slot].dst_ip = dst_ip;
    ping_states[slot].id = id;
    ping_states[slot].seq = seq;
    ping_states[slot].send_time = timer_get_ms();
    ping_states[slot].active = 1;
    ping_states[slot].timeout = 0;
    ping_states[slot].rtt = 0;

    net_send_eth(packet, total_len, dst_ip);
    kfree(packet);
    return 0;
}

void net_icmp_rx(const void *data, size_t len, uint32_t src_ip)
{
    if (len < sizeof(icmp_header_t)) return;
    const icmp_header_t *icmp = (const icmp_header_t *)data;

    if (icmp->type == ICMP_TYPE_ECHO_REQUEST && icmp->code == 0) {
        /* Echo request - send reply */
        size_t reply_len = len;
        uint8_t *reply = (uint8_t *)kmalloc(reply_len);
        if (!reply) return;

        memcpy(reply, data, reply_len);
        icmp_header_t *reply_icmp = (icmp_header_t *)reply;
        reply_icmp->type = ICMP_TYPE_ECHO_REPLY;
        reply_icmp->code = 0;
        reply_icmp->checksum = 0;
        reply_icmp->checksum = compute_checksum(reply, reply_len);

        size_t total_len = sizeof(ipv4_header_t) + reply_len;
        uint8_t *packet = (uint8_t *)kmalloc(total_len);
        if (!packet) { kfree(reply); return; }

        ipv4_header_t *ip = (ipv4_header_t *)packet;
        ip->version_ihl   = 0x45;
        ip->dscp_ecn      = 0;
        ip->total_length  = htons(total_len);
        ip->identification = htons((uint16_t)(net_random() & 0xFFFF));
        ip->flags_fragment = htons(0x4000);
        ip->ttl           = 64;
        ip->protocol      = IP_PROTO_ICMP;
        ip->header_checksum = 0;
        ip->src_ip        = net_local_ip;
        ip->dst_ip        = src_ip;
        ip->header_checksum = compute_ip_checksum(ip);

        memcpy(packet + sizeof(ipv4_header_t), reply, reply_len);
        kfree(reply);

        net_send_eth(packet, total_len, src_ip);
        kfree(packet);
    }
    else if (icmp->type == ICMP_TYPE_ECHO_REPLY && icmp->code == 0) {
        /* Echo reply - match against our ping state */
        /* Standard ICMP Echo: rest_of_header = identifier(high16) | sequence(low16) */
        uint16_t id  = ntohs((uint16_t)(icmp->rest_of_header >> 16));
        uint16_t seq = ntohs((uint16_t)(icmp->rest_of_header & 0xFFFF));

        int slot = find_ping_slot(src_ip, id);
        if (slot >= 0) {
            ping_states[slot].rtt = (uint32_t)(timer_get_ms() - ping_states[slot].send_time);
            /* Mark as received by clearing active, keep result for retrieval */
            ping_states[slot].active = 0;

            uint32_t hip = ntohl(src_ip);
            printf("[ICMP] Echo reply from %u.%u.%u.%u: seq=%u rtt=%u ms\n",
                   (hip >> 24) & 0xFF, (hip >> 16) & 0xFF,
                   (hip >> 8) & 0xFF, hip & 0xFF,
                   seq, ping_states[slot].rtt);
        }
    }
    else if (icmp->type == ICMP_TYPE_DEST_UNREACH) {
        uint32_t hip = ntohl(src_ip);
        printf("[ICMP] Destination unreachable from %u.%u.%u.%u code=%u\n",
               (hip >> 24) & 0xFF, (hip >> 16) & 0xFF,
               (hip >> 8) & 0xFF, hip & 0xFF, icmp->code);
    }
}

void net_icmp_tick(void)
{
    uint64_t now = timer_get_ms();
    for (int i = 0; i < MAX_PING_STATES; i++) {
        if (ping_states[i].active && (now - ping_states[i].send_time) > 5000) {
            /* 5 second timeout */
            uint32_t hip = ntohl(ping_states[i].dst_ip);
            printf("[ICMP] Ping to %u.%u.%u.%u seq=%u timed out\n",
                   (hip >> 24) & 0xFF, (hip >> 16) & 0xFF,
                   (hip >> 8) & 0xFF, hip & 0xFF,
                   ping_states[i].seq);
            ping_states[i].active = 0;
            ping_states[i].timeout = 1;
        }
    }
}
