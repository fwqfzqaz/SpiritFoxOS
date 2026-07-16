/*
 * socket.c - Socket API and management for SpiritFoxOS network stack
 *
 * Implements the BSD socket API, socket lookup, buffer management,
 * port allocation, and network initialization.
 */

#include "net_socket.h"
#include "net_tcp.h"
#include "net_udp.h"
#include "net_eth.h"
#include "net_utils.h"
#include "net.h"
#include "kmalloc.h"
#include "string.h"
#include "vga.h"
#include "hal.h"
#include "timer.h"
#include "rtl8139.h"
#include "net_arp.h"

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
 * Global state
 * ======================================================================== */
static net_socket_t sockets[NET_MAX_SOCKETS];
static int socket_used[NET_MAX_SOCKETS];
static port_t next_ephemeral_port = 49152;
static uint32_t tcp_initial_seq = 0x12345678;
static ipv4_addr_t loopback_ip = INADDR_LOOPBACK;

/* Network configuration (IP addresses in network byte order) */
uint32_t net_local_ip    = 0;
uint32_t net_gateway_ip  = 0;
uint32_t net_netmask     = 0;
uint8_t  net_local_mac[6] = {0};
int      net_hw_initialized = 0;

/* ========================================================================
 * Internal helper functions
 * ======================================================================== */

net_socket_t *net_get_socket(int fd)
{
    if (fd < 0 || fd >= NET_MAX_SOCKETS)
        return NULL;
    if (!socket_used[fd])
        return NULL;
    return &sockets[fd];
}

int net_alloc_socket_slot(void)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!socket_used[i]) {
            socket_used[i] = 1;
            return i;
        }
    }
    return -1;
}

void net_free_socket_slot(int fd)
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

    /* Free TCP unacknowledged segment list */
    if (s->type == SOCK_STREAM) {
        net_tcp_seg_t *seg = s->unacked_list;
        while (seg) {
            net_tcp_seg_t *next = seg->next;
            if (seg->data)
                kfree(seg->data);
            kfree(seg);
            seg = next;
        }
        s->unacked_list = NULL;
    }

    /* If this is a child socket, remove from parent's pending list */
    if (s->listen_parent) {
        net_socket_t *parent = s->listen_parent;
        for (int i = 0; i < parent->pending_count; i++) {
            if (parent->pending_conns[i] == s) {
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

/* ========================================================================
 * Socket lookup functions (non-static, used by tcp.c and udp.c)
 * ======================================================================== */

net_socket_t *net_find_socket_by_port(port_t local_port_ho, ipv4_addr_t local_ip_ho)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!socket_used[i])
            continue;
        net_socket_t *s = &sockets[i];
        if (s->local_port == local_port_ho) {
            if (s->local_ip == 0 || s->local_ip == local_ip_ho)
                return s;
        }
    }
    return NULL;
}

net_socket_t *net_find_listening_socket(port_t local_port_ho)
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

net_socket_t *net_find_tcp_socket(ipv4_addr_t local_ip, port_t local_port,
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

net_socket_t *net_find_udp_socket(port_t local_port_ho, ipv4_addr_t local_ip_ho)
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

/* ========================================================================
 * RX buffer read/write (non-static, used by tcp.c and udp.c)
 * ======================================================================== */

int net_rx_buffer_write(net_socket_t *s, const void *data, size_t len)
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

int net_rx_buffer_read(net_socket_t *s, void *data, size_t len)
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
 * Port allocation
 * ======================================================================== */

uint32_t net_get_tcp_initial_seq(void)
{
    return tcp_initial_seq;
}

port_t net_alloc_port(void)
{
    for (int attempt = 0; attempt < 16384; attempt++) {
        port_t candidate = next_ephemeral_port;
        next_ephemeral_port++;
        if (next_ephemeral_port > 65535)
            next_ephemeral_port = 49152;

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

    return 0;
}

/* ========================================================================
 * Network configuration
 * ======================================================================== */

void net_configure_ip(uint32_t ip, uint32_t gateway, uint32_t netmask)
{
    net_local_ip   = ip;
    net_gateway_ip = gateway;
    net_netmask    = netmask;

    uint32_t hip = ntohl(ip);
    printf("[NET] IP configured: %u.%u.%u.%u\n",
           (hip >> 24) & 0xFF, (hip >> 16) & 0xFF,
           (hip >> 8) & 0xFF, hip & 0xFF);
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

    net_arp_init();

    rtl8139_init();

    if (rtl8139_get_mac(net_local_mac) == 0) {
        net_hw_initialized = 1;

        if (net_local_ip == 0 && net_local_mac[0] == 0x52 &&
            net_local_mac[1] == 0x54 && net_local_mac[2] == 0x00) {
            net_local_ip    = htonl(0x0A00020F);   /* 10.0.2.15 (QEMU default) */
            net_gateway_ip  = htonl(0x0A000202);   /* 10.0.2.2 */
            net_netmask     = htonl(0xFFFFFF00);   /* 255.255.255.0 */
            printf("[NET] QEMU detected, auto-configured 10.0.2.15/24\n");
        }

        if (net_local_ip != 0) {
            uint32_t hip = ntohl(net_local_ip);
            printf("[NET] TCP/IP stack initialized (IP: %u.%u.%u.%u, MAC: %02x:%02x:%02x:%02x:%02x:%02x)\n",
                   (hip >> 24) & 0xFF, (hip >> 16) & 0xFF,
                   (hip >> 8) & 0xFF, hip & 0xFF,
                   net_local_mac[0], net_local_mac[1], net_local_mac[2],
                   net_local_mac[3], net_local_mac[4], net_local_mac[5]);
        } else {
            printf("[NET] TCP/IP stack initialized (MAC: %02x:%02x:%02x:%02x:%02x:%02x, IP unconfigured)\n",
                   net_local_mac[0], net_local_mac[1], net_local_mac[2],
                   net_local_mac[3], net_local_mac[4], net_local_mac[5]);
        }
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

    if (type == SOCK_STREAM && protocol != 0 && protocol != IP_PROTO_TCP)
        return -EPROTONOSUPPORT;
    if (type == SOCK_DGRAM && protocol != 0 && protocol != IP_PROTO_UDP)
        return -EPROTONOSUPPORT;

    int fd = net_alloc_socket_slot();
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

    /* Initialize congestion control state */
    s->cwnd = NET_TCP_MSS;
    s->ssthresh = NET_TCP_WINDOW;
    s->rto = 1000;
    s->srtt = 0;
    s->rttvar = 0;
    s->retransmit_timer = 0;
    s->dup_ack_count = 0;
    s->bytes_in_flight = 0;
    s->last_ack_sent = 0;
    s->congestion_state = 0;
    s->time_wait_expire = 0;
    s->retransmit_count = 0;
    s->unacked_list = NULL;

    s->rx_buf = (uint8_t *)kmalloc(NET_RX_BUF_SIZE);
    s->tx_buf = (uint8_t *)kmalloc(NET_TX_BUF_SIZE);
    if (!s->rx_buf || !s->tx_buf) {
        net_free_socket_slot(fd);
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
    net_socket_t *s = net_get_socket(fd);
    if (!s)
        return -EBADF;

    if (!addr)
        return -EINVAL;

    if (addr->sin_family != AF_INET)
        return -EAFNOSUPPORT;

    port_t port_ho = ntohs(addr->sin_port);
    ipv4_addr_t ip_ho = ntohl(addr->sin_addr);

    if (port_ho != 0) {
        net_socket_t *existing = net_find_socket_by_port(port_ho, ip_ho);
        if (existing && existing->fd != fd)
            return -EADDRINUSE;
    }

    if (port_ho == 0)
        port_ho = net_alloc_port();

    s->local_ip = ip_ho;
    s->local_port = port_ho;

    return 0;
}

int net_listen(int fd, int backlog)
{
    net_socket_t *s = net_get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    if (s->local_port == 0)
        return -EINVAL;

    if (backlog <= 0)
        backlog = 1;
    if (backlog > NET_TCP_BACKLOG)
        backlog = NET_TCP_BACKLOG;

    s->state = TCP_LISTEN;
    s->pending_count = 0;
    (void)backlog;

    return 0;
}

int net_accept(int fd, net_sockaddr_t *addr, socklen_t *addrlen)
{
    net_socket_t *s = net_get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    if (s->state != TCP_LISTEN)
        return -EINVAL;

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
        return -EAGAIN;
    }

    if (addr && addrlen && *addrlen >= sizeof(net_sockaddr_t)) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons(child->remote_port);
        addr->sin_addr = htonl(child->remote_ip);
        memset(addr->sin_zero, 0, sizeof(addr->sin_zero));
        *addrlen = sizeof(net_sockaddr_t);
    }

    for (int i = child_idx; i < s->pending_count - 1; i++)
        s->pending_conns[i] = s->pending_conns[i + 1];
    s->pending_count--;

    child->listen_parent = NULL;

    return child->fd;
}

int net_connect(int fd, const net_sockaddr_t *addr, socklen_t addrlen)
{
    (void)addrlen;
    net_socket_t *s = net_get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    if (!addr || addr->sin_family != AF_INET)
        return -EAFNOSUPPORT;

    if (s->local_port == 0) {
        s->local_port = net_alloc_port();
        s->local_ip = loopback_ip;
    }

    s->remote_ip = ntohl(addr->sin_addr);
    s->remote_port = ntohs(addr->sin_port);
    s->seq_num = tcp_initial_seq + net_random();
    s->ack_num = 0;

    /* Initialize TCP congestion control state */
    s->cwnd = NET_TCP_MSS;
    s->ssthresh = NET_TCP_WINDOW;
    s->rto = 1000;
    s->srtt = 0;
    s->rttvar = 0;
    s->retransmit_timer = 0;
    s->dup_ack_count = 0;
    s->bytes_in_flight = 0;
    s->last_ack_sent = 0;
    s->congestion_state = 0;
    s->unacked_list = NULL;

    s->state = TCP_SYN_SENT;
    net_tcp_send_packet(s, TCP_FLAG_SYN, NULL, 0);

    if (s->state == TCP_ESTABLISHED)
        return 0;

    return 0;
}

int net_send(int fd, const void *buf, size_t len, int flags)
{
    (void)flags;
    net_socket_t *s = net_get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT)
        return -ENOTCONN;

    if (len == 0)
        return 0;

    /* Respect congestion window: only send up to min(cwnd, window) - bytes_in_flight */
    uint32_t send_window = s->cwnd;
    if (NET_TCP_WINDOW < send_window)
        send_window = NET_TCP_WINDOW;
    if (s->bytes_in_flight >= send_window)
        send_window = 0;
    else
        send_window -= s->bytes_in_flight;

    size_t remaining = len;
    const uint8_t *ptr = (const uint8_t *)buf;
    size_t total_sent = 0;

    while (remaining > 0 && send_window > 0) {
        size_t chunk = (remaining > NET_TCP_MSS) ? NET_TCP_MSS : remaining;
        if (chunk > send_window)
            chunk = send_window;
        net_tcp_send_packet(s, TCP_FLAG_PSH | TCP_FLAG_ACK, ptr, chunk);
        ptr += chunk;
        remaining -= chunk;
        send_window -= chunk;
        total_sent += chunk;
    }

    /* Queue remaining data in tx_buf if congestion window is exhausted */
    if (remaining > 0 && s->tx_buf) {
        size_t avail = s->tx_size - s->tx_count;
        size_t to_queue = (remaining < avail) ? remaining : avail;
        for (size_t i = 0; i < to_queue; i++) {
            s->tx_buf[s->tx_write_pos] = ptr[i];
            s->tx_write_pos = (s->tx_write_pos + 1) % s->tx_size;
        }
        s->tx_count += to_queue;
        total_sent += to_queue;
    }

    return (int)total_sent;
}

int net_recv(int fd, void *buf, size_t len, int flags)
{
    (void)flags;
    net_socket_t *s = net_get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    if (s->state != TCP_ESTABLISHED && s->state != TCP_FIN_WAIT_1 &&
        s->state != TCP_FIN_WAIT_2 && s->state != TCP_CLOSE_WAIT) {
        if (s->state == TCP_CLOSED || s->state == TCP_TIME_WAIT)
            return 0;
        return -ENOTCONN;
    }

    if (s->rx_count == 0) {
        if (s->flags & SOCKF_NONBLOCK)
            return -EAGAIN;
        return -EAGAIN;
    }

    return net_rx_buffer_read(s, buf, len);
}

int net_sendto(int fd, const void *buf, size_t len, int flags,
               const net_sockaddr_t *dest_addr, socklen_t addrlen)
{
    (void)flags;
    (void)addrlen;
    net_socket_t *s = net_get_socket(fd);
    if (!s)
        return -EBADF;

    if (!dest_addr || dest_addr->sin_family != AF_INET)
        return -EAFNOSUPPORT;

    ipv4_addr_t dst_ip = ntohl(dest_addr->sin_addr);
    port_t dst_port = ntohs(dest_addr->sin_port);

    if (s->type == SOCK_STREAM) {
        return net_send(fd, buf, len, flags);
    }

    /* UDP */
    if (s->local_port == 0) {
        s->local_port = net_alloc_port();
        s->local_ip = loopback_ip;
    }

    ipv4_addr_t src_ip = s->local_ip ? s->local_ip : loopback_ip;

    net_udp_send_packet(src_ip, s->local_port, dst_ip, dst_port, buf, len);

    return (int)len;
}

int net_recvfrom(int fd, void *buf, size_t len, int flags,
                 net_sockaddr_t *src_addr, socklen_t *addrlen)
{
    (void)flags;
    net_socket_t *s = net_get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type == SOCK_STREAM) {
        return net_recv(fd, buf, len, flags);
    }

    /* UDP - check if there is a complete datagram */
    if (s->rx_count < 6) {
        if (s->flags & SOCKF_NONBLOCK)
            return -EAGAIN;
        return -EAGAIN;
    }

    uint8_t meta[6];
    net_rx_buffer_read(s, meta, 6);

    ipv4_addr_t src_ip;
    port_t src_port;
    memcpy(&src_ip, meta, 4);
    memcpy(&src_port, meta + 4, 2);

    size_t avail = s->rx_count;
    size_t to_read = (len < avail) ? len : avail;

    int n = net_rx_buffer_read(s, buf, to_read);

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
    net_socket_t *s = net_get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    if (how == 1 || how == 2) {
        if (s->state == TCP_ESTABLISHED) {
            net_tcp_send_packet(s, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_FIN_WAIT_1;
        } else if (s->state == TCP_CLOSE_WAIT) {
            net_tcp_send_packet(s, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_LAST_ACK;
        }
    }

    return 0;
}

int net_close(int fd)
{
    net_socket_t *s = net_get_socket(fd);
    if (!s)
        return -EBADF;

    if (s->type == SOCK_STREAM) {
        if (s->state == TCP_ESTABLISHED) {
            net_tcp_send_packet(s, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_FIN_WAIT_1;
        } else if (s->state == TCP_CLOSE_WAIT) {
            net_tcp_send_packet(s, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_LAST_ACK;
        }
    }

    net_free_socket_slot(fd);
    return 0;
}

int net_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
    (void)optval;
    (void)optlen;
    net_socket_t *s = net_get_socket(fd);
    if (!s)
        return -EBADF;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:
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
    net_socket_t *s = net_get_socket(fd);
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
    net_socket_t *s = net_get_socket(fd);
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
    net_socket_t *s = net_get_socket(fd);
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
