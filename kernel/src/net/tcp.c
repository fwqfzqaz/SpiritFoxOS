#include "net_tcp.h"
#include "net_eth.h"
#include "net_ip.h"
#include "net_utils.h"
#include "net_socket.h"
#include "net.h"
#include "kmalloc.h"
#include "string.h"
#include "vga.h"
#include "timer.h"
#include "hal.h"

/* TCP congestion states */
#define TCP_CC_SLOW_START       0
#define TCP_CC_CONGESTION_AVOID 1
#define TCP_CC_FAST_RECOVERY    2

/* TCP timer constants */
#define TCP_RTO_MIN     200     /* 200ms minimum RTO */
#define TCP_RTO_MAX     60000   /* 60s maximum RTO */
#define TCP_RTO_INIT    1000    /* 1s initial RTO */
#define TCP_TIMEWAIT_MS 60000   /* 60s TIME_WAIT timeout in ms */
#define TCP_MAX_RETRIES 5       /* Max retransmission attempts */

/* Advertised window based on receive buffer space */
static uint16_t tcp_advertised_window(const net_socket_t *s)
{
    uint32_t avail = s->rx_size - s->rx_count;
    if (avail > 65535) avail = 65535;
    return (uint16_t)avail;
}

uint16_t compute_tcp_checksum(ipv4_addr_t src_ip, ipv4_addr_t dst_ip,
                               const void *tcp_seg, uint16_t tcp_len)
{
    const uint16_t *ptr = (const uint16_t *)tcp_seg;
    uint32_t sum = 0;

    uint16_t src_hi = (uint16_t)(src_ip >> 16);
    uint16_t src_lo = (uint16_t)(src_ip & 0xFFFF);
    uint16_t dst_hi = (uint16_t)(dst_ip >> 16);
    uint16_t dst_lo = (uint16_t)(dst_ip & 0xFFFF);

    sum += src_hi; sum += src_lo; sum += dst_hi; sum += dst_lo;
    sum += htons((uint16_t)IP_PROTO_TCP);
    sum += htons(tcp_len);

    for (int i = 0; i < tcp_len / 2; i++) sum += ptr[i];
    if (tcp_len & 1) {
        const uint8_t *byte_ptr = (const uint8_t *)tcp_seg;
        sum += (uint16_t)byte_ptr[tcp_len - 1] << 8;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* Add segment to unacked list */
static void tcp_unacked_add(net_socket_t *s, uint32_t seq, const void *data, size_t len)
{
    net_tcp_seg_t *seg = (net_tcp_seg_t *)kmalloc(sizeof(net_tcp_seg_t));
    if (!seg) return;
    seg->data = (uint8_t *)kmalloc(len);
    if (!seg->data) { kfree(seg); return; }
    memcpy(seg->data, data, len);
    seg->seq = seq;
    seg->len = (uint32_t)len;
    seg->send_time = timer_get_ms();
    seg->retries = 0;
    seg->next = NULL;

    /* Append to end of list */
    net_tcp_seg_t **pp = &s->unacked_list;
    while (*pp) pp = &(*pp)->next;
    *pp = seg;
}

/* Remove acknowledged segments from unacked list */
static void tcp_unacked_ack(net_socket_t *s, uint32_t ack)
{
    while (s->unacked_list) {
        net_tcp_seg_t *seg = s->unacked_list;
        if (seg->seq + seg->len <= ack) {
            /* This segment is fully acknowledged */
            s->unacked_list = seg->next;
            if (seg->data) kfree(seg->data);
            kfree(seg);
        } else {
            break;
        }
    }
}

/* Free all unacked segments */
static void tcp_unacked_free(net_socket_t *s)
{
    while (s->unacked_list) {
        net_tcp_seg_t *seg = s->unacked_list;
        s->unacked_list = seg->next;
        if (seg->data) kfree(seg->data);
        kfree(seg);
    }
}

/* Update RTT estimate using Jacobson/Karels algorithm */
static void tcp_update_rtt(net_socket_t *s, uint32_t rtt_ms)
{
    if (s->srtt == 0) {
        /* First measurement */
        s->srtt = rtt_ms;
        s->rttvar = rtt_ms / 2;
    } else {
        int diff = (int)s->srtt - (int)rtt_ms;
        if (diff < 0) diff = -diff;
        s->rttvar = (3 * s->rttvar + diff) / 4;
        s->srtt = (7 * s->srtt + rtt_ms) / 8;
    }
    s->rto = s->srtt + 4 * s->rttvar;
    if (s->rto < TCP_RTO_MIN) s->rto = TCP_RTO_MIN;
    if (s->rto > TCP_RTO_MAX) s->rto = TCP_RTO_MAX;
}

/* Recalculate bytes_in_flight from unacked list */
static void tcp_recalc_in_flight(net_socket_t *s)
{
    uint32_t total = 0;
    net_tcp_seg_t *seg = s->unacked_list;
    while (seg) { total += seg->len; seg = seg->next; }
    s->bytes_in_flight = total;
}

void net_tcp_send_packet(net_socket_t *s, uint8_t tcp_flags,
                         const void *data, size_t data_len)
{
    uint8_t packet[NET_TX_BUF_SIZE];
    ipv4_header_t *ip = (ipv4_header_t *)packet;
    tcp_header_t *tcp = (tcp_header_t *)(packet + sizeof(ipv4_header_t));

    ipv4_addr_t src_ip_n = htonl(s->local_ip ? s->local_ip : INADDR_LOOPBACK);
    ipv4_addr_t dst_ip_n = htonl(s->remote_ip);

    ip->version_ihl   = 0x45;
    ip->dscp_ecn      = 0;
    ip->total_length  = htons(sizeof(ipv4_header_t) + sizeof(tcp_header_t) + data_len);
    ip->identification = htons((uint16_t)(net_random() & 0xFFFF));
    ip->flags_fragment = htons(0x4000);
    ip->ttl           = 64;
    ip->protocol      = IP_PROTO_TCP;
    ip->header_checksum = 0;
    ip->src_ip        = src_ip_n;
    ip->dst_ip        = dst_ip_n;
    ip->header_checksum = compute_ip_checksum(ip);

    memset(tcp, 0, sizeof(tcp_header_t));
    tcp->src_port = htons(s->local_port);
    tcp->dst_port = htons(s->remote_port);
    tcp->seq_num  = htonl(s->seq_num);
    tcp->ack_num  = htonl(s->ack_num);
    tcp->data_offset_flags = (sizeof(tcp_header_t) / 4) << 4;
    tcp->flags    = tcp_flags;
    tcp->window_size = htons(tcp_advertised_window(s));
    tcp->urgent_ptr  = 0;

    if (data && data_len > 0)
        memcpy((uint8_t *)(tcp + 1), data, data_len);

    uint16_t tcp_total_len = sizeof(tcp_header_t) + data_len;
    tcp->checksum = 0;
    tcp->checksum = compute_tcp_checksum(ntohl(src_ip_n), ntohl(dst_ip_n), tcp, tcp_total_len);

    size_t total_len = sizeof(ipv4_header_t) + sizeof(tcp_header_t) + data_len;

    /* Track data segments for retransmission */
    if (data_len > 0 && (tcp_flags & TCP_FLAG_ACK)) {
        tcp_unacked_add(s, s->seq_num, data, data_len);
        s->bytes_in_flight += data_len;
    }

    /* Start retransmit timer if not running */
    if (s->unacked_list && s->retransmit_timer == 0) {
        s->retransmit_timer = timer_get_ms() + s->rto;
    }

    /* SYN or FIN each consume one sequence number */
    if (tcp_flags & TCP_FLAG_SYN) s->seq_num++;
    if (tcp_flags & TCP_FLAG_FIN) s->seq_num++;
    s->seq_num += data_len;

    s->last_ack_sent = s->ack_num;

    net_send_eth(packet, total_len, dst_ip_n);
}

/* Process incoming TCP segment */
void net_tcp_rx(const ipv4_header_t *ip, const void *tcp_data, size_t ip_payload_len)
{
    if (ip_payload_len < sizeof(tcp_header_t)) return;
    const tcp_header_t *tcp = (const tcp_header_t *)tcp_data;

    port_t dst_port_ho = ntohs(tcp->dst_port);
    port_t src_port_ho = ntohs(tcp->src_port);
    ipv4_addr_t dst_ip_ho = ntohl(ip->dst_ip);
    ipv4_addr_t src_ip_ho = ntohl(ip->src_ip);
    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t flags = tcp->flags;

    uint8_t data_offset = (tcp->data_offset_flags >> 4) * 4;
    size_t header_len = data_offset;
    const void *payload = (const uint8_t *)tcp + header_len;
    size_t payload_len = ip_payload_len - header_len;

    net_socket_t *s = net_find_tcp_socket(dst_ip_ho, dst_port_ho, src_ip_ho, src_port_ho);

    if (!s) {
        s = net_find_listening_socket(dst_port_ho);
        if (!s) {
            /* Send RST */
            net_socket_t rst_sock;
            memset(&rst_sock, 0, sizeof(rst_sock));
            rst_sock.local_ip = dst_ip_ho;
            rst_sock.local_port = dst_port_ho;
            rst_sock.remote_ip = src_ip_ho;
            rst_sock.remote_port = src_port_ho;
            rst_sock.seq_num = 0;
            rst_sock.ack_num = seq + (flags & TCP_FLAG_SYN ? 1 : 0) + payload_len;
            rst_sock.ack_num = ack;  /* Use ack from the incoming packet for RST */
            net_tcp_send_packet(&rst_sock, TCP_FLAG_RST | TCP_FLAG_ACK, NULL, 0);
            return;
        }

        if (flags & TCP_FLAG_SYN) {
            if (s->pending_count >= NET_TCP_BACKLOG) return;

            int child_fd = net_alloc_socket_slot();
            if (child_fd < 0) return;

            net_socket_t *child = net_get_socket(child_fd);
            if (!child) return;

            child->fd = child_fd;
            child->domain = AF_INET;
            child->type = SOCK_STREAM;
            child->protocol = 0;
            child->state = TCP_SYN_RECEIVED;
            child->flags = 0;
            child->local_ip = dst_ip_ho;
            child->local_port = dst_port_ho;
            child->remote_ip = src_ip_ho;
            child->remote_port = src_port_ho;
            child->seq_num = net_get_tcp_initial_seq() + net_random();
            child->ack_num = seq + 1;
            child->remote_seq = seq + 1;

            /* Initialize TCP reliability state */
            child->cwnd = NET_TCP_MSS;
            child->ssthresh = NET_TCP_WINDOW;
            child->rto = TCP_RTO_INIT;
            child->srtt = 0;
            child->rttvar = 0;
            child->retransmit_timer = 0;
            child->dup_ack_count = 0;
            child->bytes_in_flight = 0;
            child->last_ack_sent = child->ack_num;
            child->congestion_state = TCP_CC_SLOW_START;
            child->time_wait_expire = 0;
            child->retransmit_count = 0;
            child->unacked_list = NULL;

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
                net_free_socket_slot(child_fd);
                return;
            }

            s->pending_conns[s->pending_count++] = child;
            net_tcp_send_packet(child, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
            return;
        }
        return;
    }

    /* Matched socket - process by state */
    switch (s->state) {
    case TCP_SYN_SENT:
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
            s->ack_num = seq + 1;
            s->remote_seq = seq + 1;
            net_tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_ESTABLISHED;

            /* Initialize congestion control for active open */
            s->cwnd = NET_TCP_MSS;
            s->ssthresh = NET_TCP_WINDOW;
            s->rto = TCP_RTO_INIT;
            s->srtt = 0;
            s->rttvar = 0;
            s->congestion_state = TCP_CC_SLOW_START;
        } else if (flags & TCP_FLAG_RST) {
            s->state = TCP_CLOSED;
        }
        break;

    case TCP_SYN_RECEIVED:
        if (flags & TCP_FLAG_ACK) {
            s->state = TCP_ESTABLISHED;
        } else if (flags & TCP_FLAG_RST) {
            s->state = TCP_CLOSED;
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_FLAG_RST) {
            s->state = TCP_CLOSED;
            tcp_unacked_free(s);
            break;
        }

        /* Process ACK - handle acknowledgments and congestion control */
        if (flags & TCP_FLAG_ACK) {
            if (ack > s->last_ack_sent || (s->unacked_list && ack > s->unacked_list->seq)) {
                /* New ACK - update RTT and remove acknowledged segments */
                uint32_t prev_in_flight = s->bytes_in_flight;

                /* Check if we can measure RTT (first unacked segment was acked) */
                if (s->unacked_list && ack >= s->unacked_list->seq + s->unacked_list->len) {
                    uint32_t rtt = (uint32_t)(timer_get_ms() - s->unacked_list->send_time);
                    tcp_update_rtt(s, rtt);
                }

                tcp_unacked_ack(s, ack);
                tcp_recalc_in_flight(s);

                uint32_t acked_bytes = prev_in_flight - s->bytes_in_flight;

                /* Congestion control */
                if (s->congestion_state == TCP_CC_SLOW_START) {
                    /* Slow start: increase cwnd by MSS per ACK */
                    s->cwnd += acked_bytes;
                    if (s->cwnd >= s->ssthresh) {
                        s->congestion_state = TCP_CC_CONGESTION_AVOID;
                    }
                } else if (s->congestion_state == TCP_CC_CONGESTION_AVOID) {
                    /* Congestion avoidance: increase cwnd by MSS*MSS/cwnd per ACK */
                    if (s->cwnd > 0)
                        s->cwnd += (NET_TCP_MSS * NET_TCP_MSS) / s->cwnd;
                } else if (s->congestion_state == TCP_CC_FAST_RECOVERY) {
                    /* Fast recovery: inflate cwnd by the acked data */
                    s->cwnd += acked_bytes;
                    /* If all data outstanding is acked, end fast recovery */
                    if (s->bytes_in_flight == 0 || ack >= s->ssthresh) {
                        s->congestion_state = TCP_CC_CONGESTION_AVOID;
                        s->cwnd = s->ssthresh;
                    }
                }

                /* Reset duplicate ACK count */
                s->dup_ack_count = 0;

                /* Reset retransmit timer if there's still outstanding data */
                if (s->unacked_list) {
                    s->retransmit_timer = timer_get_ms() + s->rto;
                } else {
                    s->retransmit_timer = 0;
                }
            } else if (ack == s->last_ack_sent) {
                /* Duplicate ACK */
                s->dup_ack_count++;

                if (s->congestion_state == TCP_CC_FAST_RECOVERY) {
                    /* Fast recovery: inflate window for each duplicate ACK */
                    s->cwnd += NET_TCP_MSS;
                } else if (s->dup_ack_count >= 3) {
                    /* Fast retransmit */
                    printf("[TCP] Fast retransmit triggered (dup_ack=%u)\n", s->dup_ack_count);
                    s->ssthresh = (s->cwnd / 2 > 2 * NET_TCP_MSS) ? s->cwnd / 2 : 2 * NET_TCP_MSS;
                    s->cwnd = s->ssthresh + 3 * NET_TCP_MSS;
                    s->congestion_state = TCP_CC_FAST_RECOVERY;

                    /* Retransmit first unacked segment */
                    if (s->unacked_list) {
                        net_tcp_seg_t *seg = s->unacked_list;
                        /* Rebuild and send the segment */
                        uint8_t tcp_flags_new = TCP_FLAG_PSH | TCP_FLAG_ACK;
                        /* Temporarily set seq_num for retransmit */
                        uint32_t saved_seq = s->seq_num;
                        s->seq_num = seg->seq;
                        net_tcp_send_packet(s, tcp_flags_new, seg->data, seg->len);
                        s->seq_num = saved_seq;
                        /* Note: the retransmitted segment will be added to unacked again,
                         * but that's acceptable for simplicity */
                    }
                }
            }
        }

        /* Process incoming data */
        if (payload_len > 0) {
            /* Check sequence number - accept if it matches expected */
            if (seq == s->remote_seq) {
                net_rx_buffer_write(s, payload, payload_len);
                s->ack_num = seq + payload_len;
                s->remote_seq = seq + payload_len;
                net_tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            } else if (seq > s->remote_seq) {
                /* Future data - send ACK with expected seq */
                net_tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            }
            /* else: out-of-order, just ACK what we have */
        }

        /* Process FIN */
        if (flags & TCP_FLAG_FIN) {
            s->ack_num = seq + 1;
            s->remote_seq = seq + 1;
            net_tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_CLOSE_WAIT;
        }
        break;

    case TCP_FIN_WAIT_1:
        if (flags & TCP_FLAG_ACK) {
            if (flags & TCP_FLAG_FIN) {
                s->ack_num = seq + 1;
                net_tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
                s->state = TCP_TIME_WAIT;
                s->time_wait_expire = timer_get_ms() + TCP_TIMEWAIT_MS;
            } else {
                s->state = TCP_FIN_WAIT_2;
            }
        } else if (flags & TCP_FLAG_FIN) {
            s->ack_num = seq + 1;
            net_tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_CLOSING;
        }
        break;

    case TCP_FIN_WAIT_2:
        if (flags & TCP_FLAG_FIN) {
            s->ack_num = seq + 1;
            net_tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_TIME_WAIT;
            s->time_wait_expire = timer_get_ms() + TCP_TIMEWAIT_MS;
        }
        if (payload_len > 0) {
            net_rx_buffer_write(s, payload, payload_len);
            s->ack_num = seq + payload_len;
            s->remote_seq = seq + payload_len;
            net_tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        }
        break;

    case TCP_CLOSING:
        if (flags & TCP_FLAG_ACK) s->state = TCP_TIME_WAIT;
        s->time_wait_expire = timer_get_ms() + TCP_TIMEWAIT_MS;
        break;

    case TCP_CLOSE_WAIT:
        if (flags & TCP_FLAG_FIN) {
            net_tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_FLAG_ACK) {
            s->state = TCP_CLOSED;
            tcp_unacked_free(s);
        }
        break;

    case TCP_TIME_WAIT:
        if (flags & TCP_FLAG_FIN) {
            net_tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        }
        break;

    default:
        break;
    }
}

/* Periodic TCP timer - called from system tick */
void net_tcp_tick(void)
{
    uint64_t now = timer_get_ms();

    /* We need to iterate all sockets - get them from socket.c */
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *s = net_get_socket(i);
        if (!s) continue;
        if (s->type != SOCK_STREAM) continue;

        /* Handle retransmission timeout */
        if (s->retransmit_timer > 0 && now >= s->retransmit_timer) {
            if (s->unacked_list) {
                s->retransmit_count++;
                if (s->retransmit_count > TCP_MAX_RETRIES) {
                    printf("[TCP] Max retransmissions reached, closing connection\n");
                    s->state = TCP_CLOSED;
                    tcp_unacked_free(s);
                    s->retransmit_timer = 0;
                    continue;
                }

                printf("[TCP] Retransmission timeout (retry=%u, rto=%u ms)\n",
                       s->retransmit_count, s->rto);

                /* On RTO: back off, reduce cwnd */
                s->rto *= 2;
                if (s->rto > TCP_RTO_MAX) s->rto = TCP_RTO_MAX;

                /* Congestion control on timeout: go back to slow start */
                s->ssthresh = (s->cwnd / 2 > 2 * NET_TCP_MSS) ? s->cwnd / 2 : 2 * NET_TCP_MSS;
                s->cwnd = NET_TCP_MSS;
                s->congestion_state = TCP_CC_SLOW_START;

                /* Retransmit first unacked segment */
                net_tcp_seg_t *seg = s->unacked_list;
                if (seg) {
                    uint32_t saved_seq = s->seq_num;
                    s->seq_num = seg->seq;
                    /* Don't add to unacked again for retransmit */
                    uint8_t tcp_flags = TCP_FLAG_PSH | TCP_FLAG_ACK;
                    if (seg->len == 0) tcp_flags = TCP_FLAG_ACK;

                    /* Manually construct and send (skip unacked_add) */
                    uint8_t packet[NET_TX_BUF_SIZE];
                    ipv4_header_t *ip_hdr = (ipv4_header_t *)packet;
                    tcp_header_t *tcp_hdr = (tcp_header_t *)(packet + sizeof(ipv4_header_t));

                    ipv4_addr_t src_ip_n = htonl(s->local_ip ? s->local_ip : INADDR_LOOPBACK);
                    ipv4_addr_t dst_ip_n = htonl(s->remote_ip);

                    ip_hdr->version_ihl   = 0x45;
                    ip_hdr->dscp_ecn      = 0;
                    ip_hdr->total_length  = htons(sizeof(ipv4_header_t) + sizeof(tcp_header_t) + seg->len);
                    ip_hdr->identification = htons((uint16_t)(net_random() & 0xFFFF));
                    ip_hdr->flags_fragment = htons(0x4000);
                    ip_hdr->ttl           = 64;
                    ip_hdr->protocol      = IP_PROTO_TCP;
                    ip_hdr->header_checksum = 0;
                    ip_hdr->src_ip        = src_ip_n;
                    ip_hdr->dst_ip        = dst_ip_n;
                    ip_hdr->header_checksum = compute_ip_checksum(ip_hdr);

                    memset(tcp_hdr, 0, sizeof(tcp_header_t));
                    tcp_hdr->src_port = htons(s->local_port);
                    tcp_hdr->dst_port = htons(s->remote_port);
                    tcp_hdr->seq_num  = htonl(seg->seq);
                    tcp_hdr->ack_num  = htonl(s->ack_num);
                    tcp_hdr->data_offset_flags = (sizeof(tcp_header_t) / 4) << 4;
                    tcp_hdr->flags    = tcp_flags;
                    tcp_hdr->window_size = htons(tcp_advertised_window(s));
                    tcp_hdr->urgent_ptr  = 0;

                    if (seg->data && seg->len > 0)
                        memcpy((uint8_t *)(tcp_hdr + 1), seg->data, seg->len);

                    uint16_t tcp_total_len = sizeof(tcp_header_t) + seg->len;
                    tcp_hdr->checksum = 0;
                    tcp_hdr->checksum = compute_tcp_checksum(ntohl(src_ip_n), ntohl(dst_ip_n), tcp_hdr, tcp_total_len);

                    size_t total_len = sizeof(ipv4_header_t) + sizeof(tcp_header_t) + seg->len;
                    net_send_eth(packet, total_len, dst_ip_n);

                    s->seq_num = saved_seq;
                }

                /* Reset timer with backed-off RTO */
                s->retransmit_timer = now + s->rto;
            } else {
                s->retransmit_timer = 0;
            }
        }

        /* Handle TIME_WAIT expiry */
        if (s->state == TCP_TIME_WAIT && s->time_wait_expire > 0 && now >= s->time_wait_expire) {
            s->state = TCP_CLOSED;
            s->time_wait_expire = 0;
            tcp_unacked_free(s);
        }
    }
}
