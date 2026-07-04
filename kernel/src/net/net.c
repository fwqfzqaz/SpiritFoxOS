/*
 * net.c - TCP/IP network stack for SpiritFoxOS
 *
 * Implements a basic TCP/IP stack with loopback interface,
 * TCP state machine (3-way handshake, data transfer, FIN close),
 * UDP send/receive, and socket buffer management.
 */

#include "net.h"
#include "kmalloc.h"
#include "memory.h"
#include "string.h"
#include "vga.h"
#include "timer.h"
#include "hal.h"
#include "rtl8139.h"

/* ========================================================================
 * Error codes (match syscall.c definitions)
 * ======================================================================== */
#define EINVAL      22
#define ENOMEM      12
#define EADDRINUSE  98
#define ECONNREFUSED 111
#define ENOTCONN    107
#define EAGAIN      11
#define EOPNOTSUPP  95
#define EAFNOSUPPORT 97
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EBADF       9
#define EPIPE       32

/* ========================================================================
 * Byte-order helpers (x86_64 is little-endian, network is big-endian)
 * ======================================================================== */
static inline uint16_t htons(uint16_t v) {
    return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF);
}
static inline uint16_t ntohs(uint16_t v) {
    return htons(v);
}
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | (((v >> 8) & 0xFF) << 16) |
           (((v >> 16) & 0xFF) << 8) | ((v >> 24) & 0xFF);
}
static inline uint32_t ntohl(uint32_t v) {
    return htonl(v);
}

/* ========================================================================
 * Global state
 * ======================================================================== */
static net_socket_t sockets[NET_MAX_SOCKETS];
static int socket_used[NET_MAX_SOCKETS];
static port_t next_ephemeral_port = 49152;
static ipv4_addr_t loopback_ip = INADDR_LOOPBACK; /* 127.0.0.1 host order */
static uint32_t tcp_initial_seq = 0x12345678;     /* Starting ISN */

/* Network configuration (IP addresses in network byte order) */
uint32_t net_local_ip    = 0x0A00020F;   /* 10.0.2.15 (QEMU user-mode default) */
uint32_t net_gateway_ip  = 0x0A000202;   /* 10.0.2.2 */
uint32_t net_netmask     = 0xFFFFFF00;   /* 255.255.255.0 */
uint8_t  net_local_mac[6] = {0};
int      net_hw_initialized = 0;

/* ARP cache */
typedef struct {
    uint32_t ip;        /* Network byte order */
    uint8_t  mac[6];
    uint64_t expire;    /* Expiry timestamp in ms */
} arp_entry_t;

static arp_entry_t arp_cache[16];

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

/* Simple pseudo-random using timer */
static uint32_t net_random(void)
{
    uint64_t t = timer_get_ms();
    t = ((t * 1103515245ULL + 12345ULL) >> 16) & 0x7FFFFFFF;
    return (uint32_t)t;
}

/* Compute IP header checksum */
static uint16_t compute_ip_checksum(const ipv4_header_t *hdr)
{
    const uint16_t *ptr = (const uint16_t *)hdr;
    uint32_t sum = 0;
    int len = ((hdr->version_ihl & 0x0F) * 4) / 2;

    for (int i = 0; i < len; i++) {
        sum += ptr[i];
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)(~sum);
}

/* Compute TCP checksum with pseudo-header */
static uint16_t compute_tcp_checksum(ipv4_addr_t src_ip, ipv4_addr_t dst_ip,
                                      const void *tcp_seg, uint16_t tcp_len)
{
    const uint16_t *ptr = (const uint16_t *)tcp_seg;
    uint32_t sum = 0;

    /* Pseudo-header */
    uint16_t src_hi = (uint16_t)(src_ip >> 16);
    uint16_t src_lo = (uint16_t)(src_ip & 0xFFFF);
    uint16_t dst_hi = (uint16_t)(dst_ip >> 16);
    uint16_t dst_lo = (uint16_t)(dst_ip & 0xFFFF);

    sum += src_hi;
    sum += src_lo;
    sum += dst_hi;
    sum += dst_lo;
    sum += htons((uint16_t)IP_PROTO_TCP);
    sum += htons(tcp_len);

    /* TCP header + data */
    for (int i = 0; i < tcp_len / 2; i++) {
        sum += ptr[i];
    }

    /* Handle odd byte */
    if (tcp_len & 1) {
        const uint8_t *byte_ptr = (const uint8_t *)tcp_seg;
        sum += (uint16_t)byte_ptr[tcp_len - 1] << 8;
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)(~sum);
}

/* Compute UDP checksum with pseudo-header */
static uint16_t compute_udp_checksum(ipv4_addr_t src_ip, ipv4_addr_t dst_ip,
                                      const void *udp_seg, uint16_t udp_len)
{
    const uint16_t *ptr = (const uint16_t *)udp_seg;
    uint32_t sum = 0;

    uint16_t src_hi = (uint16_t)(src_ip >> 16);
    uint16_t src_lo = (uint16_t)(src_ip & 0xFFFF);
    uint16_t dst_hi = (uint16_t)(dst_ip >> 16);
    uint16_t dst_lo = (uint16_t)(dst_ip & 0xFFFF);

    sum += src_hi;
    sum += src_lo;
    sum += dst_hi;
    sum += dst_lo;
    sum += htons((uint16_t)IP_PROTO_UDP);
    sum += htons(udp_len);

    for (int i = 0; i < udp_len / 2; i++) {
        sum += ptr[i];
    }

    if (udp_len & 1) {
        const uint8_t *byte_ptr = (const uint8_t *)udp_seg;
        sum += (uint16_t)byte_ptr[udp_len - 1] << 8;
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)(~sum);
}

/* Find a socket by local port and local IP */
static net_socket_t *find_socket_by_port(port_t local_port_ho, ipv4_addr_t local_ip_ho)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!socket_used[i])
            continue;
        net_socket_t *s = &sockets[i];
        if (s->local_port == local_port_ho) {
            /* Match if IP is INADDR_ANY (0) or exact match */
            if (s->local_ip == 0 || s->local_ip == local_ip_ho)
                return s;
        }
    }
    return NULL;
}

/* Find a listening socket by port */
static net_socket_t *find_listening_socket(port_t local_port_ho)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!socket_used[i])
            continue;
        net_socket_t *s = &sockets[i];
        if (s->type == SOCK_STREAM && s->state == TCP_LISTEN &&
            s->local_port == local_port_ho) {
            return s;
        }
    }
    return NULL;
}

/* Find a TCP socket by 4-tuple */
static net_socket_t *find_tcp_socket(ipv4_addr_t local_ip, port_t local_port,
                                      ipv4_addr_t remote_ip, port_t remote_port)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!socket_used[i])
            continue;
        net_socket_t *s = &sockets[i];
        if (s->type != SOCK_STREAM)
            continue;
        if (s->local_port == local_port &&
            (s->local_ip == 0 || s->local_ip == local_ip) &&
            s->remote_port == remote_port &&
            s->remote_ip == remote_ip) {
            return s;
        }
    }
    return NULL;
}

/* Find a UDP socket by local port */
static net_socket_t *find_udp_socket(port_t local_port_ho, ipv4_addr_t local_ip_ho)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!socket_used[i])
            continue;
        net_socket_t *s = &sockets[i];
        if (s->type == SOCK_DGRAM &&
            s->local_port == local_port_ho &&
            (s->local_ip == 0 || s->local_ip == local_ip_ho)) {
            return s;
        }
    }
    return NULL;
}

/* Get socket by fd */
static net_socket_t *get_socket(int fd)
{
    if (fd < 0 || fd >= NET_MAX_SOCKETS)
        return NULL;
    if (!socket_used[fd])
        return NULL;
    return &sockets[fd];
}

/* Allocate a free socket slot */
static int alloc_socket_slot(void)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!socket_used[i]) {
            socket_used[i] = 1;
            return i;
        }
    }
    return -1;
}

/* Free a socket slot and its resources */
static void free_socket_slot(int fd)
{
    if (fd < 0 || fd >= NET_MAX_SOCKETS)
        return;

    net_socket_t *s = &sockets[fd];

    if (s->rx_buf) {
        kfree(s->rx_buf);
        s->rx_buf = NULL;
    }
    if (s->tx_buf) {
        kfree(s->tx_buf);
        s->tx_buf = NULL;
    }

    /* Remove from parent's pending list if we're a child */
    if (s->listen_parent) {
        net_socket_t *parent = s->listen_parent;
        for (int i = 0; i < parent->pending_count; i++) {
            if (parent->pending_conns[i] == s) {
                /* Shift remaining entries */
                for (int j = i; j < parent->pending_count - 1; j++)
                    parent->pending_conns[j] = parent->pending_conns[j + 1];
                parent->pending_count--;
                break;
            }
        }
        s->listen_parent = NULL;
    }

    memset(s, 0, sizeof(net_socket_t));
    socket_used[fd] = 0;
}

/* Write data into a socket's circular RX buffer */
static int rx_buffer_write(net_socket_t *s, const void *data, size_t len)
{
    if (!s->rx_buf || len == 0)
        return 0;

    size_t avail = s->rx_size - s->rx_count;
    size_t to_write = (len < avail) ? len : avail;

    const uint8_t *src = (const uint8_t *)data;
    for (size_t i = 0; i < to_write; i++) {
        s->rx_buf[s->rx_write_pos] = src[i];
        s->rx_write_pos = (s->rx_write_pos + 1) % s->rx_size;
    }
    s->rx_count += to_write;

    return (int)to_write;
}

/* Read data from a socket's circular RX buffer */
static int rx_buffer_read(net_socket_t *s, void *data, size_t len)
{
    if (!s->rx_buf || s->rx_count == 0)
        return 0;

    size_t to_read = (len < s->rx_count) ? len : s->rx_count;

    uint8_t *dst = (uint8_t *)data;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = s->rx_buf[s->rx_read_pos];
        s->rx_read_pos = (s->rx_read_pos + 1) % s->rx_size;
    }
    s->rx_count -= to_read;

    return (int)to_read;
}

/* ========================================================================
 * Packet construction and transmission
 * ======================================================================== */

/* Build and send a TCP segment over loopback */
static void tcp_send_packet(net_socket_t *s, uint8_t tcp_flags,
                            const void *data, size_t data_len)
{
    uint8_t packet[NET_TX_BUF_SIZE];
    ipv4_header_t *ip = (ipv4_header_t *)packet;
    tcp_header_t *tcp = (tcp_header_t *)(packet + sizeof(ipv4_header_t));

    /* IPs in network byte order for the wire */
    ipv4_addr_t src_ip_n = htonl(s->local_ip ? s->local_ip : loopback_ip);
    ipv4_addr_t dst_ip_n = htonl(s->remote_ip);

    /* IP header */
    ip->version_ihl   = 0x45;      /* IPv4, IHL=5 (20 bytes) */
    ip->dscp_ecn      = 0;
    ip->total_length  = htons(sizeof(ipv4_header_t) + sizeof(tcp_header_t) + data_len);
    ip->identification = htons((uint16_t)(net_random() & 0xFFFF));
    ip->flags_fragment = htons(0x4000); /* Don't fragment */
    ip->ttl           = 64;
    ip->protocol      = IP_PROTO_TCP;
    ip->header_checksum = 0;
    ip->src_ip        = src_ip_n;
    ip->dst_ip        = dst_ip_n;
    ip->header_checksum = compute_ip_checksum(ip);

    /* TCP header */
    memset(tcp, 0, sizeof(tcp_header_t));
    tcp->src_port = htons(s->local_port);
    tcp->dst_port = htons(s->remote_port);
    tcp->seq_num  = htonl(s->seq_num);
    tcp->ack_num  = htonl(s->ack_num);
    tcp->data_offset_flags = (sizeof(tcp_header_t) / 4) << 4;
    tcp->flags    = tcp_flags;
    tcp->window_size = htons(NET_TCP_WINDOW);
    tcp->urgent_ptr  = 0;

    /* Copy payload */
    if (data && data_len > 0) {
        memcpy((uint8_t *)(tcp + 1), data, data_len);
    }

    /* Compute TCP checksum */
    tcp->checksum = 0;
    uint16_t tcp_total_len = sizeof(tcp_header_t) + data_len;
    tcp->checksum = compute_tcp_checksum(ntohl(src_ip_n), ntohl(dst_ip_n),
                                          tcp, tcp_total_len);

    size_t total_len = sizeof(ipv4_header_t) + sizeof(tcp_header_t) + data_len;

    /* Advance seq for SYN or FIN (they each consume 1 seq number) */
    if (tcp_flags & TCP_FLAG_SYN)
        s->seq_num++;
    if (tcp_flags & TCP_FLAG_FIN)
        s->seq_num++;
    s->seq_num += data_len;

    /* Send via Ethernet (handles loopback for 127.x.x.x) */
    net_send_eth(packet, total_len, dst_ip_n);
}

/* Send a UDP datagram over loopback */
static void udp_send_packet(ipv4_addr_t src_ip_ho, port_t src_port_ho,
                             ipv4_addr_t dst_ip_ho, port_t dst_port_ho,
                             const void *data, size_t data_len)
{
    uint8_t packet[NET_TX_BUF_SIZE];
    ipv4_header_t *ip = (ipv4_header_t *)packet;
    udp_header_t *udp = (udp_header_t *)(packet + sizeof(ipv4_header_t));

    ipv4_addr_t src_ip_n = htonl(src_ip_ho ? src_ip_ho : loopback_ip);
    ipv4_addr_t dst_ip_n = htonl(dst_ip_ho);

    /* IP header */
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

    /* UDP header */
    udp->src_port = htons(src_port_ho);
    udp->dst_port = htons(dst_port_ho);
    udp->length   = htons(sizeof(udp_header_t) + data_len);
    udp->checksum = 0;

    /* Copy payload */
    if (data && data_len > 0) {
        memcpy((uint8_t *)(udp + 1), data, data_len);
    }

    /* Compute UDP checksum */
    uint16_t udp_total_len = sizeof(udp_header_t) + data_len;
    udp->checksum = compute_udp_checksum(ntohl(src_ip_n), ntohl(dst_ip_n),
                                          udp, udp_total_len);

    size_t total_len = sizeof(ipv4_header_t) + sizeof(udp_header_t) + data_len;
    net_send_eth(packet, total_len, dst_ip_n);
}

/* ========================================================================
 * TCP state machine processing
 * ======================================================================== */

/* Process an incoming TCP segment */
static void process_tcp_packet(const ipv4_header_t *ip, const void *tcp_data, size_t ip_payload_len)
{
    if (ip_payload_len < sizeof(tcp_header_t))
        return;

    const tcp_header_t *tcp = (const tcp_header_t *)tcp_data;

    port_t dst_port_ho = ntohs(tcp->dst_port);
    port_t src_port_ho = ntohs(tcp->src_port);
    ipv4_addr_t dst_ip_ho = ntohl(ip->dst_ip);
    ipv4_addr_t src_ip_ho = ntohl(ip->src_ip);
    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t flags = tcp->flags;

    /* Data offset in 32-bit words */
    uint8_t data_offset = (tcp->data_offset_flags >> 4) * 4;
    size_t header_len = data_offset;
    const void *payload = (const uint8_t *)tcp + header_len;
    size_t payload_len = ip_payload_len - header_len;

    /* Try to find an established/connection socket by 4-tuple */
    net_socket_t *s = find_tcp_socket(dst_ip_ho, dst_port_ho, src_ip_ho, src_port_ho);

    if (!s) {
        /* Check for a listening socket */
        s = find_listening_socket(dst_port_ho);
        if (!s) {
            /* No listener - send RST */
            /* Build a minimal RST response */
            net_socket_t rst_sock;
            memset(&rst_sock, 0, sizeof(rst_sock));
            rst_sock.local_ip  = dst_ip_ho;
            rst_sock.local_port = dst_port_ho;
            rst_sock.remote_ip = src_ip_ho;
            rst_sock.remote_port = src_port_ho;
            rst_sock.seq_num  = 0;
            rst_sock.ack_num  = seq + (flags & TCP_FLAG_SYN ? 1 : 0) + payload_len;
            tcp_send_packet(&rst_sock, TCP_FLAG_RST | TCP_FLAG_ACK, NULL, 0);
            return;
        }

        /* SYN to a listening socket - create child socket */
        if (flags & TCP_FLAG_SYN) {
            if (s->pending_count >= NET_TCP_BACKLOG) {
                /* Backlog full, silently drop */
                return;
            }

            int child_fd = alloc_socket_slot();
            if (child_fd < 0)
                return;

            net_socket_t *child = &sockets[child_fd];
            child->fd = child_fd;
            child->domain = AF_INET;
            child->type = SOCK_STREAM;
            child->protocol = 0;
            child->state = TCP_SYN_RECEIVED;
            child->flags = 0;
            child->local_ip  = dst_ip_ho;
            child->local_port = dst_port_ho;
            child->remote_ip = src_ip_ho;
            child->remote_port = src_port_ho;
            child->seq_num = tcp_initial_seq + net_random();
            child->ack_num = seq + 1;
            child->remote_seq = seq + 1;
            child->rx_buf = (uint8_t *)kmalloc(NET_RX_BUF_SIZE);
            child->rx_size = NET_RX_BUF_SIZE;
            child->rx_read_pos = 0;
            child->rx_write_pos = 0;
            child->rx_count = 0;
            child->tx_buf = (uint8_t *)kmalloc(NET_TX_BUF_SIZE);
            child->tx_size = NET_TX_BUF_SIZE;
            child->tx_read_pos = 0;
            child->tx_write_pos = 0;
            child->tx_count = 0;
            child->listen_parent = s;
            child->next_pending = NULL;
            child->blocking_pid = -1;

            if (!child->rx_buf || !child->tx_buf) {
                free_socket_slot(child_fd);
                return;
            }

            /* Add to parent's pending list */
            s->pending_conns[s->pending_count++] = child;

            /* Send SYN-ACK */
            tcp_send_packet(child, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);

            return;
        }

        /* Non-SYN to a listening socket with no matching child - drop */
        return;
    }

    /* We have a matching socket - process by state */
    switch (s->state) {
    case TCP_SYN_SENT:
        /* We sent SYN, expecting SYN-ACK */
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
            s->ack_num = seq + 1;
            s->remote_seq = seq + 1;
            /* Send ACK to complete handshake */
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_ESTABLISHED;
        } else if (flags & TCP_FLAG_RST) {
            s->state = TCP_CLOSED;
        }
        break;

    case TCP_SYN_RECEIVED:
        /* We sent SYN-ACK, expecting ACK */
        if (flags & TCP_FLAG_ACK) {
            s->state = TCP_ESTABLISHED;
            /* Any data with the ACK can be processed below */
        } else if (flags & TCP_FLAG_RST) {
            /* Clean up this child socket */
            s->state = TCP_CLOSED;
        }
        break;

    case TCP_ESTABLISHED:
        /* Process incoming data */
        if (flags & TCP_FLAG_RST) {
            s->state = TCP_CLOSED;
            break;
        }

        if (payload_len > 0) {
            /* Write to RX buffer */
            rx_buffer_write(s, payload, payload_len);

            /* Update ACK number */
            s->ack_num = seq + payload_len;
            s->remote_seq = seq + payload_len;

            /* Send ACK */
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        }

        /* Handle FIN */
        if (flags & TCP_FLAG_FIN) {
            s->ack_num = seq + 1;
            s->remote_seq = seq + 1;
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_CLOSE_WAIT;
        }
        break;

    case TCP_FIN_WAIT_1:
        if (flags & TCP_FLAG_ACK) {
            /* Our FIN was acknowledged */
            if (flags & TCP_FLAG_FIN) {
                /* Simultaneous close - both sent FIN */
                s->ack_num = seq + 1;
                tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
                s->state = TCP_TIME_WAIT;
            } else {
                s->state = TCP_FIN_WAIT_2;
            }
        } else if (flags & TCP_FLAG_FIN) {
            /* Simultaneous close */
            s->ack_num = seq + 1;
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_CLOSING;
        }
        break;

    case TCP_FIN_WAIT_2:
        /* Waiting for remote FIN */
        if (flags & TCP_FLAG_FIN) {
            s->ack_num = seq + 1;
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_TIME_WAIT;
        }
        /* Can still receive data in FIN_WAIT_2 */
        if (payload_len > 0) {
            rx_buffer_write(s, payload, payload_len);
            s->ack_num = seq + payload_len;
            s->remote_seq = seq + payload_len;
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        }
        break;

    case TCP_CLOSING:
        if (flags & TCP_FLAG_ACK) {
            s->state = TCP_TIME_WAIT;
        }
        break;

    case TCP_CLOSE_WAIT:
        /* Application should call close/shutdown */
        if (flags & TCP_FLAG_FIN) {
            /* Duplicate FIN, re-ACK */
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_FLAG_ACK) {
            s->state = TCP_CLOSED;
        }
        break;

    case TCP_TIME_WAIT:
        /* Re-ACK any retransmitted FINs */
        if (flags & TCP_FLAG_FIN) {
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        }
        /* For simplicity, immediately transition to CLOSED */
        s->state = TCP_CLOSED;
        break;

    default:
        break;
    }
}

/* Process an incoming UDP datagram */
static void process_udp_packet(const ipv4_header_t *ip, const void *udp_data, size_t ip_payload_len)
{
    if (ip_payload_len < sizeof(udp_header_t))
        return;

    const udp_header_t *udp = (const udp_header_t *)udp_data;

    port_t dst_port_ho = ntohs(udp->dst_port);
    port_t src_port_ho = ntohs(udp->src_port);
    ipv4_addr_t dst_ip_ho = ntohl(ip->dst_ip);
    ipv4_addr_t src_ip_ho = ntohl(ip->src_ip);

    uint16_t udp_len = ntohs(udp->length);
    size_t payload_len = udp_len - sizeof(udp_header_t);
    const void *payload = (const uint8_t *)udp + sizeof(udp_header_t);

    /* Find the target UDP socket */
    net_socket_t *s = find_udp_socket(dst_port_ho, dst_ip_ho);
    if (!s)
        return; /* Silently drop */

    /* Write a simple header + payload into RX buffer so recvfrom can
     * extract the source address. Format:
     *   [4 bytes src_ip][2 bytes src_port][payload]  */
    uint8_t meta[6];
    memcpy(meta, &src_ip_ho, 4);
    memcpy(meta + 4, &src_port_ho, 2);

    uint64_t iflags = hal_save_interrupts();
    rx_buffer_write(s, meta, 6);
    rx_buffer_write(s, payload, payload_len);
    hal_restore_interrupts(iflags);
}

/* ========================================================================
 * ARP protocol
 * ======================================================================== */

void net_arp_init(void)
{
    memset(arp_cache, 0, sizeof(arp_cache));
}

/* Compute ICMP-style checksum over arbitrary data (used for ICMP and ARP) */
static uint16_t compute_checksum(const void *data, size_t len)
{
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i < len / 2; i++) {
        sum += ptr[i];
    }
    if (len & 1) {
        const uint8_t *byte_ptr = (const uint8_t *)data;
        sum += (uint16_t)byte_ptr[len - 1] << 8;
    }
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static void arp_cache_insert(uint32_t ip, const uint8_t mac[6])
{
    uint64_t now = timer_get_ms();

    /* Update existing entry or find empty slot */
    for (int i = 0; i < 16; i++) {
        if (arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].expire = now + 60000; /* 60 seconds */
            return;
        }
    }
    /* Find empty or expired slot */
    for (int i = 0; i < 16; i++) {
        if (arp_cache[i].ip == 0 || arp_cache[i].expire <= now) {
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].expire = now + 60000;
            return;
        }
    }
    /* Overwrite oldest (first expired or first entry) */
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

/* Send an ARP request for the given IP */
static void arp_send_request(uint32_t target_ip)
{
    uint8_t frame[14 + sizeof(arp_packet_t)];
    eth_header_t *eth = (eth_header_t *)frame;
    arp_packet_t *arp = (arp_packet_t *)(frame + 14);

    /* Ethernet header: broadcast destination */
    memset(eth->dst_mac, 0xFF, 6);
    memcpy(eth->src_mac, net_local_mac, 6);
    eth->ethertype = htons(0x0806);

    /* ARP payload */
    arp->htype = htons(1);          /* Ethernet */
    arp->ptype = htons(0x0800);     /* IPv4 */
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(1);          /* Request */
    memcpy(arp->sha, net_local_mac, 6);
    arp->spa   = net_local_ip;      /* Already in network byte order */
    memset(arp->tha, 0x00, 6);
    arp->tpa   = target_ip;

    rtl8139_send(frame, sizeof(frame));
}

/* Send an ARP reply */
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
    arp->oper  = htons(2);          /* Reply */
    memcpy(arp->sha, net_local_mac, 6);
    arp->spa   = net_local_ip;
    memcpy(arp->tha, target_mac, 6);
    arp->tpa   = target_ip;

    rtl8139_send(frame, sizeof(frame));
}

void net_arp_rx(const void *data, size_t len)
{
    if (len < sizeof(arp_packet_t))
        return;

    const arp_packet_t *arp = (const arp_packet_t *)data;

    /* Only handle Ethernet + IPv4 ARP */
    if (ntohs(arp->htype) != 1 || ntohs(arp->ptype) != 0x0800)
        return;
    if (arp->hlen != 6 || arp->plen != 4)
        return;

    uint16_t oper = ntohs(arp->oper);

    /* If this is a reply, cache the sender's MAC */
    if (oper == 2) {
        if (arp->tpa == net_local_ip) {
            arp_cache_insert(arp->spa, arp->sha);
        }
        return;
    }

    /* If this is a request for our IP, send a reply */
    if (oper == 1) {
        if (arp->tpa == net_local_ip) {
            /* Also cache the sender's info */
            arp_cache_insert(arp->spa, arp->sha);
            arp_send_reply(arp->spa, arp->sha);
        }
    }
}

int net_arp_resolve(uint32_t ip, uint8_t *mac_out)
{
    /* Check cache first */
    if (arp_cache_lookup(ip, mac_out) == 0)
        return 0;

    /* Not in cache - send ARP request and poll with timeout */
    arp_send_request(ip);

    uint64_t deadline = timer_get_ms() + 1000; /* 1 second timeout */
    while (timer_get_ms() < deadline) {
        /* Brief busy-wait; allow IRQ to process the reply */
        hal_io_wait();
        if (arp_cache_lookup(ip, mac_out) == 0)
            return 0;
    }

    return -1; /* Not resolved */
}

/* ========================================================================
 * Ethernet send (wraps IP packet with Ethernet header, handles ARP)
 * ======================================================================== */

void net_send_eth(const void *ip_packet, size_t ip_len, uint32_t dst_ip)
{
    if (!ip_packet || ip_len == 0)
        return;

    /* Check if destination is loopback (127.x.x.x) */
    uint8_t first_byte = (ntohl(dst_ip) >> 24) & 0xFF;
    if (first_byte == 127) {
        net_loopback_tx(ip_packet, ip_len);
        return;
    }

    if (!net_hw_initialized) {
        /* No hardware - fall back to loopback */
        net_loopback_tx(ip_packet, ip_len);
        return;
    }

    /* Determine next-hop: if on same subnet, target is dst_ip; else gateway */
    uint32_t next_hop_ip;
    if ((dst_ip & net_netmask) == (net_local_ip & net_netmask)) {
        next_hop_ip = dst_ip;
    } else {
        next_hop_ip = net_gateway_ip;
    }

    /* Resolve destination MAC via ARP */
    uint8_t dst_mac[6];
    if (net_arp_resolve(next_hop_ip, dst_mac) != 0) {
        return; /* Could not resolve MAC address */
    }

    /* Construct Ethernet frame */
    size_t frame_len = sizeof(eth_header_t) + ip_len;
    uint8_t *frame = (uint8_t *)kmalloc(frame_len);
    if (!frame)
        return;

    eth_header_t *eth = (eth_header_t *)frame;
    memcpy(eth->dst_mac, dst_mac, 6);
    memcpy(eth->src_mac, net_local_mac, 6);
    eth->ethertype = htons(0x0800);

    memcpy(frame + sizeof(eth_header_t), ip_packet, ip_len);

    rtl8139_send(frame, frame_len);

    kfree(frame);
}

/* ========================================================================
 * ICMP protocol
 * ======================================================================== */

void net_icmp_rx(const void *data, size_t len, uint32_t src_ip)
{
    if (len < sizeof(icmp_header_t))
        return;

    const icmp_header_t *icmp = (const icmp_header_t *)data;

    /* Handle Echo Request (ping) */
    if (icmp->type == 8 && icmp->code == 0) {
        /* Build Echo Reply */
        size_t reply_len = len;
        uint8_t *reply = (uint8_t *)kmalloc(reply_len);
        if (!reply)
            return;

        memcpy(reply, data, reply_len);

        icmp_header_t *reply_icmp = (icmp_header_t *)reply;
        reply_icmp->type = 0;    /* Echo Reply */
        reply_icmp->code = 0;
        reply_icmp->checksum = 0;
        reply_icmp->checksum = compute_checksum(reply, reply_len);

        /* Build the IP + ICMP packet for sending */
        size_t total_len = sizeof(ipv4_header_t) + reply_len;
        uint8_t *packet = (uint8_t *)kmalloc(total_len);
        if (!packet) {
            kfree(reply);
            return;
        }

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
}

/* ========================================================================
 * Public API - Initialization
 * ======================================================================== */

void net_init(void)
{
    memset(sockets, 0, sizeof(sockets));
    memset(socket_used, 0, sizeof(socket_used));
    next_ephemeral_port = 49152;
    tcp_initial_seq = net_random();

    /* Initialize ARP cache */
    net_arp_init();

    /* Initialize RTL8139 NIC */
    rtl8139_init();

    /* Get MAC address from NIC */
    if (rtl8139_get_mac(net_local_mac) == 0) {
        net_hw_initialized = 1;
        printf("[NET] TCP/IP stack initialized (IP: 10.0.2.15, MAC: %02x:%02x:%02x:%02x:%02x:%02x)\n",
               net_local_mac[0], net_local_mac[1], net_local_mac[2],
               net_local_mac[3], net_local_mac[4], net_local_mac[5]);
    } else {
        printf("[NET] TCP/IP stack initialized (loopback: 127.0.0.1, no NIC)\n");
    }
}

/* ========================================================================
 * Public API - Socket operations
 * ======================================================================== */

int net_socket(int domain, int type, int protocol)
{
    if (domain != AF_INET)
        return -EAFNOSUPPORT;

    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return -ESOCKTNOSUPPORT;

    if (protocol != 0 && protocol != IP_PROTO_TCP && protocol != IP_PROTO_UDP)
        return -EPROTONOSUPPORT;

    /* Validate protocol matches type */
    if (type == SOCK_STREAM && protocol != 0 && protocol != IP_PROTO_TCP)
        return -EPROTONOSUPPORT;
    if (type == SOCK_DGRAM && protocol != 0 && protocol != IP_PROTO_UDP)
        return -EPROTONOSUPPORT;

    int fd = alloc_socket_slot();
    if (fd < 0)
        return -ENOMEM;

    net_socket_t *s = &sockets[fd];
    s->fd = fd;
    s->domain = domain;
    s->type = type;
    s->protocol = (type == SOCK_STREAM) ? IP_PROTO_TCP : IP_PROTO_UDP;
    s->state = TCP_CLOSED;
    s->flags = 0;
    s->local_ip = 0;
    s->local_port = 0;
    s->remote_ip = 0;
    s->remote_port = 0;
    s->seq_num = tcp_initial_seq + net_random();
    s->ack_num = 0;
    s->remote_seq = 0;
    s->listen_parent = NULL;
    s->pending_count = 0;
    s->next_pending = NULL;
    s->blocking_pid = -1;

    /* Allocate buffers */
    s->rx_buf = (uint8_t *)kmalloc(NET_RX_BUF_SIZE);
    s->tx_buf = (uint8_t *)kmalloc(NET_TX_BUF_SIZE);
    if (!s->rx_buf || !s->tx_buf) {
        free_socket_slot(fd);
        return -ENOMEM;
    }
    s->rx_size = NET_RX_BUF_SIZE;
    s->rx_read_pos = 0;
    s->rx_write_pos = 0;
    s->rx_count = 0;
    s->tx_size = NET_TX_BUF_SIZE;
    s->tx_read_pos = 0;
    s->tx_write_pos = 0;
    s->tx_count = 0;

    return fd;
}

int net_bind(int fd, const net_sockaddr_t *addr, socklen_t addrlen)
{
    (void)addrlen;
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (!addr)
        return -EINVAL;

    if (addr->sin_family != AF_INET)
        return -EAFNOSUPPORT;

    port_t port_ho = ntohs(addr->sin_port);
    ipv4_addr_t ip_ho = ntohl(addr->sin_addr);

    /* Check if port is already bound */
    if (port_ho != 0) {
        net_socket_t *existing = find_socket_by_port(port_ho, ip_ho);
        if (existing && existing->fd != fd)
            return -EADDRINUSE;
    }

    /* If port is 0, allocate one */
    if (port_ho == 0)
        port_ho = net_alloc_port();

    s->local_ip = ip_ho;
    s->local_port = port_ho;

    return 0;
}

int net_listen(int fd, int backlog)
{
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    if (s->local_port == 0)
        return -EINVAL;

    /* Clamp backlog */
    if (backlog <= 0)
        backlog = 1;
    if (backlog > NET_TCP_BACKLOG)
        backlog = NET_TCP_BACKLOG;

    s->state = TCP_LISTEN;
    s->pending_count = 0;
    (void)backlog; /* Stored implicitly via NET_TCP_BACKLOG */

    return 0;
}

int net_accept(int fd, net_sockaddr_t *addr, socklen_t *addrlen)
{
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    if (s->state != TCP_LISTEN)
        return -EINVAL;

    /* Look for a pending connection that is ESTABLISHED */
    net_socket_t *child = NULL;
    int child_idx = -1;
    for (int i = 0; i < s->pending_count; i++) {
        if (s->pending_conns[i]->state == TCP_ESTABLISHED) {
            child = s->pending_conns[i];
            child_idx = i;
            break;
        }
    }

    if (!child) {
        if (s->flags & SOCKF_NONBLOCK)
            return -EAGAIN;
        /* In a real kernel we'd block here. For now, return EAGAIN. */
        return -EAGAIN;
    }

    /* Fill in the remote address */
    if (addr && addrlen && *addrlen >= sizeof(net_sockaddr_t)) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons(child->remote_port);
        addr->sin_addr = htonl(child->remote_ip);
        memset(addr->sin_zero, 0, sizeof(addr->sin_zero));
        *addrlen = sizeof(net_sockaddr_t);
    }

    /* Remove from parent's pending list */
    for (int i = child_idx; i < s->pending_count - 1; i++)
        s->pending_conns[i] = s->pending_conns[i + 1];
    s->pending_count--;

    child->listen_parent = NULL;

    return child->fd;
}

int net_connect(int fd, const net_sockaddr_t *addr, socklen_t addrlen)
{
    (void)addrlen;
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    if (!addr || addr->sin_family != AF_INET)
        return -EAFNOSUPPORT;

    /* Assign local port if not bound */
    if (s->local_port == 0) {
        s->local_port = net_alloc_port();
        s->local_ip = loopback_ip;
    }

    s->remote_ip = ntohl(addr->sin_addr);
    s->remote_port = ntohs(addr->sin_port);
    s->seq_num = tcp_initial_seq + net_random();
    s->ack_num = 0;

    /* Send SYN */
    s->state = TCP_SYN_SENT;
    tcp_send_packet(s, TCP_FLAG_SYN, NULL, 0);

    /* In a real kernel, we'd wait for the SYN-ACK. For this
     * loopback implementation, the response is delivered
     * immediately via net_loopback_tx -> net_rx_ipv4.
     * Check if the handshake completed. */
    if (s->state == TCP_ESTABLISHED)
        return 0;

    /* If still SYN_SENT, the connection is pending.
     * Return 0 to indicate connection initiated (non-blocking). */
    return 0;
}

int net_send(int fd, const void *buf, size_t len, int flags)
{
    (void)flags;
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT)
        return -ENOTCONN;

    if (len == 0)
        return 0;

    /* Fragment into MSS-sized segments */
    size_t remaining = len;
    const uint8_t *ptr = (const uint8_t *)buf;

    while (remaining > 0) {
        size_t chunk = (remaining > NET_TCP_MSS) ? NET_TCP_MSS : remaining;
        tcp_send_packet(s, TCP_FLAG_PSH | TCP_FLAG_ACK, ptr, chunk);
        ptr += chunk;
        remaining -= chunk;
    }

    return (int)len;
}

int net_recv(int fd, void *buf, size_t len, int flags)
{
    (void)flags;
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    if (s->state != TCP_ESTABLISHED && s->state != TCP_FIN_WAIT_1 &&
        s->state != TCP_FIN_WAIT_2 && s->state != TCP_CLOSE_WAIT) {
        if (s->state == TCP_CLOSED || s->state == TCP_TIME_WAIT)
            return 0; /* EOF */
        return -ENOTCONN;
    }

    if (s->rx_count == 0) {
        if (s->flags & SOCKF_NONBLOCK)
            return -EAGAIN;
        /* Would block - return EAGAIN for simplicity */
        return -EAGAIN;
    }

    return rx_buffer_read(s, buf, len);
}

int net_sendto(int fd, const void *buf, size_t len, int flags,
               const net_sockaddr_t *dest_addr, socklen_t addrlen)
{
    (void)flags;
    (void)addrlen;
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (!dest_addr || dest_addr->sin_family != AF_INET)
        return -EAFNOSUPPORT;

    ipv4_addr_t dst_ip = ntohl(dest_addr->sin_addr);
    port_t dst_port = ntohs(dest_addr->sin_port);

    if (s->type == SOCK_STREAM) {
        /* For TCP, sendto behaves like send if connected */
        return net_send(fd, buf, len, flags);
    }

    /* UDP */
    if (s->local_port == 0) {
        s->local_port = net_alloc_port();
        s->local_ip = loopback_ip;
    }

    ipv4_addr_t src_ip = s->local_ip ? s->local_ip : loopback_ip;

    udp_send_packet(src_ip, s->local_port, dst_ip, dst_port, buf, len);

    return (int)len;
}

int net_recvfrom(int fd, void *buf, size_t len, int flags,
                 net_sockaddr_t *src_addr, socklen_t *addrlen)
{
    (void)flags;
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type == SOCK_STREAM) {
        /* For TCP, recvfrom behaves like recv */
        return net_recv(fd, buf, len, flags);
    }

    /* UDP - check if there is a complete datagram */
    if (s->rx_count < 6) {
        if (s->flags & SOCKF_NONBLOCK)
            return -EAGAIN;
        return -EAGAIN;
    }

    /* Read the 6-byte metadata header */
    uint8_t meta[6];
    rx_buffer_read(s, meta, 6);

    ipv4_addr_t src_ip;
    port_t src_port;
    memcpy(&src_ip, meta, 4);
    memcpy(&src_port, meta + 4, 2);

    /* Determine how much payload data is available for this datagram.
     * We stored all the payload right after the header, so read up to len. */
    size_t avail = s->rx_count;
    size_t to_read = (len < avail) ? len : avail;

    int n = rx_buffer_read(s, buf, to_read);

    if (src_addr && addrlen && *addrlen >= sizeof(net_sockaddr_t)) {
        src_addr->sin_family = AF_INET;
        src_addr->sin_port = htons(src_port);
        src_addr->sin_addr = htonl(src_ip);
        memset(src_addr->sin_zero, 0, sizeof(src_addr->sin_zero));
        *addrlen = sizeof(net_sockaddr_t);
    }

    return n;
}

int net_shutdown(int fd, int how)
{
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    /* how: 0=recv, 1=send, 2=both */
    if (how == 1 || how == 2) {
        /* Send FIN */
        if (s->state == TCP_ESTABLISHED) {
            tcp_send_packet(s, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_FIN_WAIT_1;
        } else if (s->state == TCP_CLOSE_WAIT) {
            tcp_send_packet(s, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_LAST_ACK;
        }
    }

    return 0;
}

int net_close(int fd)
{
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    /* Send FIN for TCP if still connected */
    if (s->type == SOCK_STREAM) {
        if (s->state == TCP_ESTABLISHED) {
            tcp_send_packet(s, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_FIN_WAIT_1;
        } else if (s->state == TCP_CLOSE_WAIT) {
            tcp_send_packet(s, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_LAST_ACK;
        }
    }

    free_socket_slot(fd);
    return 0;
}

int net_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
    (void)optval;
    (void)optlen;
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:
            /* No-op, always allow reuse for simplicity */
            return 0;
        case SO_BINDTODEVICE:
            return 0;
        default:
            break;
        }
    }

    return -EOPNOTSUPP;
}

int net_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR: {
            int val = 1;
            if (optval && optlen && *optlen >= sizeof(int)) {
                memcpy(optval, &val, sizeof(int));
                *optlen = sizeof(int);
            }
            return 0;
        }
        default:
            break;
        }
    }

    return -EOPNOTSUPP;
}

int net_getsockname(int fd, net_sockaddr_t *addr, socklen_t *addrlen)
{
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (!addr || !addrlen)
        return -EINVAL;

    addr->sin_family = AF_INET;
    addr->sin_port = htons(s->local_port);
    addr->sin_addr = htonl(s->local_ip);
    memset(addr->sin_zero, 0, sizeof(addr->sin_zero));
    *addrlen = sizeof(net_sockaddr_t);

    return 0;
}

int net_getpeername(int fd, net_sockaddr_t *addr, socklen_t *addrlen)
{
    net_socket_t *s = get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->remote_port == 0)
        return -ENOTCONN;

    if (!addr || !addrlen)
        return -EINVAL;

    addr->sin_family = AF_INET;
    addr->sin_port = htons(s->remote_port);
    addr->sin_addr = htonl(s->remote_ip);
    memset(addr->sin_zero, 0, sizeof(addr->sin_zero));
    *addrlen = sizeof(net_sockaddr_t);

    return 0;
}

/* ========================================================================
 * Port allocation
 * ======================================================================== */

port_t net_alloc_port(void)
{
    /* Ephemeral port range: 49152-65535 */
    for (int attempt = 0; attempt < 16384; attempt++) {
        port_t candidate = next_ephemeral_port;
        next_ephemeral_port++;
        if (next_ephemeral_port > 65535)
            next_ephemeral_port = 49152;

        /* Check if port is in use */
        int in_use = 0;
        for (int i = 0; i < NET_MAX_SOCKETS; i++) {
            if (socket_used[i] && sockets[i].local_port == candidate) {
                in_use = 1;
                break;
            }
        }

        if (!in_use)
            return candidate;
    }

    return 0; /* No free ports */
}

/* ========================================================================
 * Loopback interface
 * ======================================================================== */

void net_loopback_tx(const void *data, size_t len)
{
    if (!data || len < sizeof(ipv4_header_t))
        return;

    const ipv4_header_t *ip = (const ipv4_header_t *)data;

    /* Verify this is an IPv4 packet */
    if ((ip->version_ihl >> 4) != 4)
        return;

    /* Verify IP checksum */
    ipv4_header_t ip_copy = *ip;
    ip_copy.header_checksum = 0;
    uint16_t verify = compute_ip_checksum(&ip_copy);
    if (verify != 0 && ip->header_checksum != 0) {
        /* Checksum mismatch - but for loopback, be lenient */
    }

    /* Deliver the packet to the local IP stack */
    net_rx_ipv4(data, len);
}

/* ========================================================================
 * IP receive handler
 * ======================================================================== */

void net_rx_ipv4(const void *data, size_t len)
{
    if (!data || len < sizeof(ipv4_header_t))
        return;

    const ipv4_header_t *ip = (const ipv4_header_t *)data;

    /* Verify IPv4 */
    if ((ip->version_ihl >> 4) != 4)
        return;

    uint16_t total_length = ntohs(ip->total_length);
    if (total_length > len)
        return; /* Truncated packet */

    /* Determine header length */
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    if (ihl < sizeof(ipv4_header_t))
        return;

    const void *payload = (const uint8_t *)data + ihl;
    size_t payload_len = total_length - ihl;

    /* Dispatch by protocol */
    switch (ip->protocol) {
    case IP_PROTO_TCP:
        process_tcp_packet(ip, payload, payload_len);
        break;
    case IP_PROTO_UDP:
        process_udp_packet(ip, payload, payload_len);
        break;
    case IP_PROTO_ICMP:
        net_icmp_rx(payload, payload_len, ip->src_ip);
        break;
    default:
        break;
    }
}
