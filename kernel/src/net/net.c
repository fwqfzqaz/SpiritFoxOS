/*
 * net.c - SpiritFoxOS TCP/IP 网络协议栈
 *
 * 实现基本的 TCP/IP 协议栈，包含回环接口、
 * TCP 状态机（三次握手、数据传输、FIN 关闭）、
 * UDP 收发和套接字缓冲区管理。
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
 * 错误码（与 syscall.c 定义匹配）
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
 * 字节序辅助函数（x86_64 为小端序，网络为大端序）
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
 * 全局状态
 * ======================================================================== */
static net_socket_t sockets[NET_MAX_SOCKETS];
static int socket_used[NET_MAX_SOCKETS];
static port_t next_ephemeral_port = 49152;
static ipv4_addr_t loopback_ip = INADDR_LOOPBACK; /* 127.0.0.1 主机字节序 */
static uint32_t tcp_initial_seq = 0x12345678;     /* 起始 ISN */

/* 网络配置（IP 地址为网络字节序）
 * 默认值为 0（未配置），需要通过 DHCP 或手动配置。
 * QEMU 用户模式网络的默认值为 10.0.2.15/24，网关 10.0.2.2，
 * 可通过启动参数 net_ip/net_gw/net_mask 配置。 */
uint32_t net_local_ip    = 0;    /* 未配置 - 等待 DHCP 或手动设置 */
uint32_t net_gateway_ip  = 0;    /* 未配置 */
uint32_t net_netmask     = 0;    /* 未配置 */
uint8_t  net_local_mac[6] = {0};
int      net_hw_initialized = 0;

/* ARP 缓存 */
typedef struct {
    uint32_t ip;        /* 网络字节序 */
    uint8_t  mac[6];
    uint64_t expire;    /* 过期时间戳（毫秒） */
} arp_entry_t;

static arp_entry_t arp_cache[16];

/* ========================================================================
 * 内部辅助函数
 * ======================================================================== */

/* 基于定时器的简单伪随机数 */
static uint32_t net_random(void)
{
    uint64_t t = timer_get_ms();
    t = ((t * 1103515245ULL + 12345ULL) >> 16) & 0x7FFFFFFF;
    return (uint32_t)t;
}

/* 计算 IP 头校验和 */
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

/* 计算带伪首部的 TCP 校验和 */
static uint16_t compute_tcp_checksum(ipv4_addr_t src_ip, ipv4_addr_t dst_ip,
                                      const void *tcp_seg, uint16_t tcp_len)
{
    const uint16_t *ptr = (const uint16_t *)tcp_seg;
    uint32_t sum = 0;

    /* 伪首部 */
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

    /* TCP 头 + 数据 */
    for (int i = 0; i < tcp_len / 2; i++) {
        sum += ptr[i];
    }

    /* 处理奇数字节 */
    if (tcp_len & 1) {
        const uint8_t *byte_ptr = (const uint8_t *)tcp_seg;
        sum += (uint16_t)byte_ptr[tcp_len - 1] << 8;
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)(~sum);
}

/* 计算带伪首部的 UDP 校验和 */
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

/* 根据本地端口和本地 IP 查找套接字 */
static net_socket_t *find_socket_by_port(port_t local_port_ho, ipv4_addr_t local_ip_ho)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!socket_used[i])
            continue;
        net_socket_t *s = &sockets[i];
        if (s->local_port == local_port_ho) {
            /* IP 为 INADDR_ANY (0) 或完全匹配时命中 */
            if (s->local_ip == 0 || s->local_ip == local_ip_ho)
                return s;
        }
    }
    return NULL;
}

/* 根据端口查找监听套接字 */
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

/* 根据四元组查找 TCP 套接字 */
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

/* 根据本地端口查找 UDP 套接字 */
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

/* 根据文件描述符获取套接字 */
static net_socket_t *get_socket(int fd)
{
    if (fd < 0 || fd >= NET_MAX_SOCKETS)
        return NULL;
    if (!socket_used[fd])
        return NULL;
    return &sockets[fd];
}

/* 分配空闲套接字槽位 */
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

/* 释放套接字槽位及其资源 */
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

    /* 如果是子套接字，从父套接字的待处理列表中移除 */
    if (s->listen_parent) {
        net_socket_t *parent = s->listen_parent;
        for (int i = 0; i < parent->pending_count; i++) {
            if (parent->pending_conns[i] == s) {
                /* 移动剩余条目 */
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

/* 将数据写入套接字的环形接收缓冲区 */
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

/* 从套接字的环形接收缓冲区读取数据 */
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
 * 数据包构造与传输
 * ======================================================================== */

/* 构造并通过回环发送 TCP 报文段 */
static void tcp_send_packet(net_socket_t *s, uint8_t tcp_flags,
                            const void *data, size_t data_len)
{
    uint8_t packet[NET_TX_BUF_SIZE];
    ipv4_header_t *ip = (ipv4_header_t *)packet;
    tcp_header_t *tcp = (tcp_header_t *)(packet + sizeof(ipv4_header_t));

    /* IP 地址转为网络字节序用于线路传输 */
    ipv4_addr_t src_ip_n = htonl(s->local_ip ? s->local_ip : loopback_ip);
    ipv4_addr_t dst_ip_n = htonl(s->remote_ip);

    /* IP 头部 */
    ip->version_ihl   = 0x45;      /* IPv4, IHL=5（20 字节） */
    ip->dscp_ecn      = 0;
    ip->total_length  = htons(sizeof(ipv4_header_t) + sizeof(tcp_header_t) + data_len);
    ip->identification = htons((uint16_t)(net_random() & 0xFFFF));
    ip->flags_fragment = htons(0x4000); /* 禁止分片 */
    ip->ttl           = 64;
    ip->protocol      = IP_PROTO_TCP;
    ip->header_checksum = 0;
    ip->src_ip        = src_ip_n;
    ip->dst_ip        = dst_ip_n;
    ip->header_checksum = compute_ip_checksum(ip);

    /* TCP 头部 */
    memset(tcp, 0, sizeof(tcp_header_t));
    tcp->src_port = htons(s->local_port);
    tcp->dst_port = htons(s->remote_port);
    tcp->seq_num  = htonl(s->seq_num);
    tcp->ack_num  = htonl(s->ack_num);
    tcp->data_offset_flags = (sizeof(tcp_header_t) / 4) << 4;
    tcp->flags    = tcp_flags;
    tcp->window_size = htons(NET_TCP_WINDOW);
    tcp->urgent_ptr  = 0;

    /* 复制载荷 */
    if (data && data_len > 0) {
        memcpy((uint8_t *)(tcp + 1), data, data_len);
    }

    /* 计算 TCP 校验和 */
    tcp->checksum = 0;
    uint16_t tcp_total_len = sizeof(tcp_header_t) + data_len;
    tcp->checksum = compute_tcp_checksum(ntohl(src_ip_n), ntohl(dst_ip_n),
                                          tcp, tcp_total_len);

    size_t total_len = sizeof(ipv4_header_t) + sizeof(tcp_header_t) + data_len;

    /* SYN 或 FIN 各占用一个序列号，推进序列号 */
    if (tcp_flags & TCP_FLAG_SYN)
        s->seq_num++;
    if (tcp_flags & TCP_FLAG_FIN)
        s->seq_num++;
    s->seq_num += data_len;

    /* 通过以太网发送（对 127.x.x.x 处理回环） */
    net_send_eth(packet, total_len, dst_ip_n);
}

/* 通过回环发送 UDP 数据报 */
static void udp_send_packet(ipv4_addr_t src_ip_ho, port_t src_port_ho,
                             ipv4_addr_t dst_ip_ho, port_t dst_port_ho,
                             const void *data, size_t data_len)
{
    uint8_t packet[NET_TX_BUF_SIZE];
    ipv4_header_t *ip = (ipv4_header_t *)packet;
    udp_header_t *udp = (udp_header_t *)(packet + sizeof(ipv4_header_t));

    ipv4_addr_t src_ip_n = htonl(src_ip_ho ? src_ip_ho : loopback_ip);
    ipv4_addr_t dst_ip_n = htonl(dst_ip_ho);

    /* IP 头部 */
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

    /* UDP 头部 */
    udp->src_port = htons(src_port_ho);
    udp->dst_port = htons(dst_port_ho);
    udp->length   = htons(sizeof(udp_header_t) + data_len);
    udp->checksum = 0;

    /* 复制载荷 */
    if (data && data_len > 0) {
        memcpy((uint8_t *)(udp + 1), data, data_len);
    }

    /* 计算 UDP 校验和 */
    uint16_t udp_total_len = sizeof(udp_header_t) + data_len;
    udp->checksum = compute_udp_checksum(ntohl(src_ip_n), ntohl(dst_ip_n),
                                          udp, udp_total_len);

    size_t total_len = sizeof(ipv4_header_t) + sizeof(udp_header_t) + data_len;
    net_send_eth(packet, total_len, dst_ip_n);
}

/* ========================================================================
 * TCP 状态机处理
 * ======================================================================== */

/* 处理传入的 TCP 报文段 */
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

    /* 数据偏移量（以 32 位字为单位） */
    uint8_t data_offset = (tcp->data_offset_flags >> 4) * 4;
    size_t header_len = data_offset;
    const void *payload = (const uint8_t *)tcp + header_len;
    size_t payload_len = ip_payload_len - header_len;

    /* 尝试根据四元组查找已建立/已连接的套接字 */
    net_socket_t *s = find_tcp_socket(dst_ip_ho, dst_port_ho, src_ip_ho, src_port_ho);

    if (!s) {
        /* 查找监听套接字 */
        s = find_listening_socket(dst_port_ho);
        if (!s) {
            /* 无监听者 - 发送 RST */
            /* 构造最小 RST 响应 */
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

        /* SYN 到达监听套接字 - 创建子套接字 */
        if (flags & TCP_FLAG_SYN) {
            if (s->pending_count >= NET_TCP_BACKLOG) {
                /* 待处理队列已满，静默丢弃 */
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

            /* 添加到父套接字的待处理列表 */
            s->pending_conns[s->pending_count++] = child;

            /* 发送 SYN-ACK */
            tcp_send_packet(child, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);

            return;
        }

        /* 非 SYN 到达监听套接字且无匹配子套接字 - 丢弃 */
        return;
    }

    /* 找到匹配套接字 - 按状态处理 */
    switch (s->state) {
    case TCP_SYN_SENT:
        /* 已发送 SYN，期待 SYN-ACK */
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
            s->ack_num = seq + 1;
            s->remote_seq = seq + 1;
            /* 发送 ACK 完成三次握手 */
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_ESTABLISHED;
        } else if (flags & TCP_FLAG_RST) {
            s->state = TCP_CLOSED;
        }
        break;

    case TCP_SYN_RECEIVED:
        /* 已发送 SYN-ACK，期待 ACK */
        if (flags & TCP_FLAG_ACK) {
            s->state = TCP_ESTABLISHED;
            /* ACK 中携带的数据可在下方处理 */
        } else if (flags & TCP_FLAG_RST) {
            /* 清理此子套接字 */
            s->state = TCP_CLOSED;
        }
        break;

    case TCP_ESTABLISHED:
        /* 处理传入数据 */
        if (flags & TCP_FLAG_RST) {
            s->state = TCP_CLOSED;
            break;
        }

        if (payload_len > 0) {
            /* 写入接收缓冲区 */
            rx_buffer_write(s, payload, payload_len);

            /* 更新确认号 */
            s->ack_num = seq + payload_len;
            s->remote_seq = seq + payload_len;

            /* 发送 ACK */
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        }

        /* 处理 FIN */
        if (flags & TCP_FLAG_FIN) {
            s->ack_num = seq + 1;
            s->remote_seq = seq + 1;
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_CLOSE_WAIT;
        }
        break;

    case TCP_FIN_WAIT_1:
        if (flags & TCP_FLAG_ACK) {
            /* 我们的 FIN 已被确认 */
            if (flags & TCP_FLAG_FIN) {
                /* 同时关闭 - 双方都发送了 FIN */
                s->ack_num = seq + 1;
                tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
                s->state = TCP_TIME_WAIT;
            } else {
                s->state = TCP_FIN_WAIT_2;
            }
        } else if (flags & TCP_FLAG_FIN) {
            /* 同时关闭 */
            s->ack_num = seq + 1;
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_CLOSING;
        }
        break;

    case TCP_FIN_WAIT_2:
        /* 等待远端 FIN */
        if (flags & TCP_FLAG_FIN) {
            s->ack_num = seq + 1;
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_TIME_WAIT;
        }
        /* FIN_WAIT_2 状态下仍可接收数据 */
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
        /* 应用应调用 close/shutdown */
        if (flags & TCP_FLAG_FIN) {
            /* 重复 FIN，重新确认 */
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_FLAG_ACK) {
            s->state = TCP_CLOSED;
        }
        break;

    case TCP_TIME_WAIT:
        /* 对重传的 FIN 重新确认 */
        if (flags & TCP_FLAG_FIN) {
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        }
        /* 简化处理，立即转为 CLOSED 状态 */
        s->state = TCP_CLOSED;
        break;

    default:
        break;
    }
}

/* 处理传入的 UDP 数据报 */
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

    /* 查找目标 UDP 套接字 */
    net_socket_t *s = find_udp_socket(dst_port_ho, dst_ip_ho);
    if (!s)
        return; /* 静默丢弃 */

    /* 将简单的头部 + 载荷写入接收缓冲区，以便 recvfrom
     * 提取源地址。格式：
     *   [4 字节 src_ip][2 字节 src_port][载荷]  */
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

/* 计算任意数据的 ICMP 风格校验和（用于 ICMP 和 ARP） */
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

    /* 更新已有条目或查找空槽 */
    for (int i = 0; i < 16; i++) {
        if (arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].expire = now + 60000; /* 60 秒 */
            return;
        }
    }
    /* 查找空槽或已过期槽 */
    for (int i = 0; i < 16; i++) {
        if (arp_cache[i].ip == 0 || arp_cache[i].expire <= now) {
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].expire = now + 60000;
            return;
        }
    }
    /* 覆盖最旧的（最先过期或第一个条目） */
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

/* 为给定 IP 发送 ARP 请求 */
static void arp_send_request(uint32_t target_ip)
{
    uint8_t frame[14 + sizeof(arp_packet_t)];
    eth_header_t *eth = (eth_header_t *)frame;
    arp_packet_t *arp = (arp_packet_t *)(frame + 14);

    /* 以太网头部：广播目标 */
    memset(eth->dst_mac, 0xFF, 6);
    memcpy(eth->src_mac, net_local_mac, 6);
    eth->ethertype = htons(0x0806);

    /* ARP 载荷 */
    arp->htype = htons(1);          /* 以太网 */
    arp->ptype = htons(0x0800);     /* IPv4 */
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(1);          /* 请求 */
    memcpy(arp->sha, net_local_mac, 6);
    arp->spa   = net_local_ip;      /* 已为网络字节序 */
    memset(arp->tha, 0x00, 6);
    arp->tpa   = target_ip;

    rtl8139_send(frame, sizeof(frame));
}

/* 发送 ARP 应答 */
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
    arp->oper  = htons(2);          /* 应答 */
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

    /* 仅处理以太网 + IPv4 ARP */
    if (ntohs(arp->htype) != 1 || ntohs(arp->ptype) != 0x0800)
        return;
    if (arp->hlen != 6 || arp->plen != 4)
        return;

    uint16_t oper = ntohs(arp->oper);

    /* 如果是应答，缓存发送方的 MAC */
    if (oper == 2) {
        if (arp->tpa == net_local_ip) {
            arp_cache_insert(arp->spa, arp->sha);
        }
        return;
    }

    /* 如果是针对本机 IP 的请求，发送应答 */
    if (oper == 1) {
        if (arp->tpa == net_local_ip) {
            /* 同时缓存发送方信息 */
            arp_cache_insert(arp->spa, arp->sha);
            arp_send_reply(arp->spa, arp->sha);
        }
    }
}

int net_arp_resolve(uint32_t ip, uint8_t *mac_out)
{
    /* 先检查缓存 */
    if (arp_cache_lookup(ip, mac_out) == 0)
        return 0;

    /* 不在缓存中 - 发送 ARP 请求并轮询等待 */
    arp_send_request(ip);

    uint64_t deadline = timer_get_ms() + 1000; /* 1 秒超时 */
    while (timer_get_ms() < deadline) {
        /* 短暂忙等待；允许中断处理应答 */
        hal_io_wait();
        if (arp_cache_lookup(ip, mac_out) == 0)
            return 0;
    }

    return -1; /* 未解析 */
}

/* ========================================================================
 * 以太网发送（为 IP 包封装以太网头部，处理 ARP）
 * ======================================================================== */

void net_send_eth(const void *ip_packet, size_t ip_len, uint32_t dst_ip)
{
    if (!ip_packet || ip_len == 0)
        return;

    /* 检查目标是否为回环地址（127.x.x.x） */
    uint8_t first_byte = (ntohl(dst_ip) >> 24) & 0xFF;
    if (first_byte == 127) {
        net_loopback_tx(ip_packet, ip_len);
        return;
    }

    if (!net_hw_initialized) {
        /* 无硬件 - 回退到回环 */
        net_loopback_tx(ip_packet, ip_len);
        return;
    }

    /* 确定下一跳：若在同一子网则目标为 dst_ip，否则为网关 */
    uint32_t next_hop_ip;
    if ((dst_ip & net_netmask) == (net_local_ip & net_netmask)) {
        next_hop_ip = dst_ip;
    } else {
        next_hop_ip = net_gateway_ip;
    }

    /* 通过 ARP 解析目标 MAC */
    uint8_t dst_mac[6];
    if (net_arp_resolve(next_hop_ip, dst_mac) != 0) {
        return; /* 无法解析 MAC 地址 */
    }

    /* 构造以太网帧 */
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
 * ICMP 协议
 * ======================================================================== */

void net_icmp_rx(const void *data, size_t len, uint32_t src_ip)
{
    if (len < sizeof(icmp_header_t))
        return;

    const icmp_header_t *icmp = (const icmp_header_t *)data;

    /* 处理回显请求（ping） */
    if (icmp->type == 8 && icmp->code == 0) {
        /* 构造回显应答 */
        size_t reply_len = len;
        uint8_t *reply = (uint8_t *)kmalloc(reply_len);
        if (!reply)
            return;

        memcpy(reply, data, reply_len);

        icmp_header_t *reply_icmp = (icmp_header_t *)reply;
        reply_icmp->type = 0;    /* 回显应答 */
        reply_icmp->code = 0;
        reply_icmp->checksum = 0;
        reply_icmp->checksum = compute_checksum(reply, reply_len);

        /* 构造 IP + ICMP 包用于发送 */
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
 * 公共 API - 初始化
 * ======================================================================== */

/* 手动配置网络 IP 地址（网络字节序） */
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

void net_init(void)
{
    memset(sockets, 0, sizeof(sockets));
    memset(socket_used, 0, sizeof(socket_used));
    next_ephemeral_port = 49152;
    tcp_initial_seq = net_random();

    /* 初始化 ARP 缓存 */
    net_arp_init();

    /* 初始化 RTL8139 网卡 */
    rtl8139_init();

    /* 从网卡获取 MAC 地址 */
    if (rtl8139_get_mac(net_local_mac) == 0) {
        net_hw_initialized = 1;

        /* 如果 IP 未配置，检测是否运行在 QEMU 环境中。
         * QEMU 默认的 rtl8139 网卡 MAC 通常以 52:54:00 开头。
         * 如果是，自动配置 QEMU 用户模式网络的默认 IP。 */
        if (net_local_ip == 0 && net_local_mac[0] == 0x52 &&
            net_local_mac[1] == 0x54 && net_local_mac[2] == 0x00) {
            net_local_ip    = 0x0A00020F;   /* 10.0.2.15 (QEMU default) */
            net_gateway_ip  = 0x0A000202;   /* 10.0.2.2 */
            net_netmask     = 0xFFFFFF00;   /* 255.255.255.0 */
            printf("[NET] QEMU detected, auto-configured 10.0.2.15/24\n");
        }

        if (net_local_ip != 0) {
            /* 将本地 IP 转为主机字节序显示 */
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
 * 公共 API - 套接字操作
 * ======================================================================== */

int net_socket(int domain, int type, int protocol)
{
    if (domain != AF_INET)
        return -EAFNOSUPPORT;

    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return -ESOCKTNOSUPPORT;

    if (protocol != 0 && protocol != IP_PROTO_TCP && protocol != IP_PROTO_UDP)
        return -EPROTONOSUPPORT;

    /* 验证协议与类型匹配 */
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
