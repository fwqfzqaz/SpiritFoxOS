#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>

/* socklen_t */
typedef uint32_t socklen_t;

/* Ethernet types */
#define ETH_TYPE_IPV4   0x0800
#define ETH_TYPE_ARP    0x0806
#define ETH_TYPE_IPV6   0x86DD

/* IP protocols */
#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

/* Address families */
#define AF_INET         2

/* Socket types */
#define SOCK_STREAM     1
#define SOCK_DGRAM      2

/* Socket options */
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_BINDTODEVICE 25

/* INADDR_ANY */
#define INADDR_ANY      0
#define INADDR_LOOPBACK 0x7F000001

/* TCP states */
#define TCP_CLOSED       0
#define TCP_LISTEN       1
#define TCP_SYN_SENT     2
#define TCP_SYN_RECEIVED 3
#define TCP_ESTABLISHED  4
#define TCP_FIN_WAIT_1   5
#define TCP_FIN_WAIT_2   6
#define TCP_CLOSING      7
#define TCP_TIME_WAIT    8
#define TCP_CLOSE_WAIT   9
#define TCP_LAST_ACK     10

/* Socket flags */
#define SOCKF_NONBLOCK  0x01

/* Maximum values */
#define NET_MAX_SOCKETS    256
#define NET_MAX_CONNECTIONS 64
#define NET_RX_BUF_SIZE    4096
#define NET_TX_BUF_SIZE    4096
#define NET_TCP_WINDOW     65535
#define NET_TCP_MSS        1460
#define NET_TCP_BACKLOG    16

/* Ethernet header */
typedef struct __attribute__((packed)) {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;    /* Network byte order */
} eth_header_t;

/* ARP packet */
typedef struct __attribute__((packed)) {
    uint16_t htype;        /* Hardware type (1 = Ethernet) */
    uint16_t ptype;        /* Protocol type (0x0800 = IPv4) */
    uint8_t  hlen;         /* Hardware address length (6) */
    uint8_t  plen;         /* Protocol address length (4) */
    uint16_t oper;         /* Operation (1 = request, 2 = reply) */
    uint8_t  sha[6];       /* Sender hardware address */
    uint32_t spa;          /* Sender protocol address */
    uint8_t  tha[6];       /* Target hardware address */
    uint32_t tpa;          /* Target protocol address */
} arp_packet_t;

/* ICMP header */
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint32_t rest_of_header;
} icmp_header_t;

/* IPv4 address */
typedef uint32_t ipv4_addr_t;

/* Port */
typedef uint16_t port_t;

/* Socket address (IPv4) */
typedef struct {
    uint16_t    sin_family;
    port_t      sin_port;      /* Network byte order */
    ipv4_addr_t sin_addr;      /* Network byte order */
    char        sin_zero[8];
} net_sockaddr_t;

/* IPv4 header */
typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;     /* Version (4) + IHL (5) */
    uint8_t  dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t header_checksum;
    ipv4_addr_t src_ip;
    ipv4_addr_t dst_ip;
} ipv4_header_t;

/* TCP header */
typedef struct __attribute__((packed)) {
    port_t   src_port;
    port_t   dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset_flags;  /* (header_len/4) << 4 | flags */
    uint8_t  flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_header_t;

/* TCP flags */
#define TCP_FLAG_FIN  0x01
#define TCP_FLAG_SYN  0x02
#define TCP_FLAG_RST  0x04
#define TCP_FLAG_PSH  0x08
#define TCP_FLAG_ACK  0x10
#define TCP_FLAG_URG  0x20

/* UDP header */
typedef struct __attribute__((packed)) {
    port_t   src_port;
    port_t   dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_header_t;

/* Socket structure */
typedef struct net_socket {
    int         fd;             /* File descriptor index */
    int         domain;         /* AF_INET */
    int         type;           /* SOCK_STREAM or SOCK_DGRAM */
    int         protocol;
    int         state;          /* TCP state or 0 */
    uint32_t    flags;

    /* Local and remote addresses */
    ipv4_addr_t local_ip;
    port_t      local_port;
    ipv4_addr_t remote_ip;
    port_t      remote_port;

    /* TCP state */
    uint32_t    seq_num;
    uint32_t    ack_num;
    uint32_t    remote_seq;

    /* Send/receive buffers (circular) */
    uint8_t    *rx_buf;
    uint32_t    rx_read_pos;
    uint32_t    rx_write_pos;
    uint32_t    rx_count;
    uint32_t    rx_size;

    uint8_t    *tx_buf;
    uint32_t    tx_read_pos;
    uint32_t    tx_write_pos;
    uint32_t    tx_count;
    uint32_t    tx_size;

    /* Listen backlog */
    struct net_socket *listen_parent;
    struct net_socket *pending_conns[NET_TCP_BACKLOG];
    int         pending_count;

    /* Connection list for accept() */
    struct net_socket *next_pending;

    /* Process blocking on this socket */
    int         blocking_pid;
} net_socket_t;

/* Initialize the network subsystem */
void net_init(void);

/* Socket API (called from syscalls) */
int net_socket(int domain, int type, int protocol);
int net_bind(int fd, const net_sockaddr_t *addr, socklen_t addrlen);
int net_listen(int fd, int backlog);
int net_accept(int fd, net_sockaddr_t *addr, socklen_t *addrlen);
int net_connect(int fd, const net_sockaddr_t *addr, socklen_t addrlen);
int net_send(int fd, const void *buf, size_t len, int flags);
int net_recv(int fd, void *buf, size_t len, int flags);
int net_sendto(int fd, const void *buf, size_t len, int flags,
               const net_sockaddr_t *dest_addr, socklen_t addrlen);
int net_recvfrom(int fd, void *buf, size_t len, int flags,
                 net_sockaddr_t *src_addr, socklen_t *addrlen);
int net_shutdown(int fd, int how);
int net_close(int fd);
int net_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int net_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int net_getsockname(int fd, net_sockaddr_t *addr, socklen_t *addrlen);
int net_getpeername(int fd, net_sockaddr_t *addr, socklen_t *addrlen);

/* Loopback transmit (injects a packet back to the loopback interface) */
void net_loopback_tx(const void *data, size_t len);

/* Process an incoming IPv4 packet (called by NIC driver or loopback) */
void net_rx_ipv4(const void *data, size_t len);

/* Allocate a local port */
port_t net_alloc_port(void);

/* Network configuration globals */
extern uint32_t net_local_ip;
extern uint32_t net_gateway_ip;
extern uint32_t net_netmask;
extern uint8_t  net_local_mac[6];
extern int      net_hw_initialized;

/* 手动配置网络 IP（用于实体机无 DHCP 时的静态配置） */
void net_configure_ip(uint32_t ip, uint32_t gateway, uint32_t netmask);

/* ARP functions */
void net_arp_init(void);
int  net_arp_resolve(uint32_t ip, uint8_t *mac_out);
void net_arp_rx(const void *data, size_t len);

/* Ethernet send (wraps IP packet in Ethernet frame, handles ARP) */
void net_send_eth(const void *ip_packet, size_t ip_len, uint32_t dst_ip);

/* ICMP receive handler */
void net_icmp_rx(const void *data, size_t len, uint32_t src_ip);

#endif /* NET_H */
