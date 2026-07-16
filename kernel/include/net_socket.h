#ifndef NET_SOCKET_H
#define NET_SOCKET_H
#include "net.h"
int net_socket(int domain, int type, int protocol);
int net_bind(int fd, const net_sockaddr_t *addr, socklen_t addrlen);
int net_listen(int fd, int backlog);
int net_accept(int fd, net_sockaddr_t *addr, socklen_t *addrlen);
int net_connect(int fd, const net_sockaddr_t *addr, socklen_t addrlen);
int net_send(int fd, const void *buf, size_t len, int flags);
int net_recv(int fd, void *buf, size_t len, int flags);
int net_sendto(int fd, const void *buf, size_t len, int flags, const net_sockaddr_t *dest_addr, socklen_t addrlen);
int net_recvfrom(int fd, void *buf, size_t len, int flags, net_sockaddr_t *src_addr, socklen_t *addrlen);
int net_shutdown(int fd, int how);
int net_close(int fd);
int net_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int net_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int net_getsockname(int fd, net_sockaddr_t *addr, socklen_t *addrlen);
int net_getpeername(int fd, net_sockaddr_t *addr, socklen_t *addrlen);
port_t net_alloc_port(void);
void net_configure_ip(uint32_t ip, uint32_t gateway, uint32_t netmask);

/* Socket lookup (used by tcp.c, udp.c) */
net_socket_t *net_find_socket_by_port(port_t local_port_ho, ipv4_addr_t local_ip_ho);
net_socket_t *net_find_listening_socket(port_t local_port_ho);
net_socket_t *net_find_tcp_socket(ipv4_addr_t local_ip, port_t local_port,
                                  ipv4_addr_t remote_ip, port_t remote_port);
net_socket_t *net_find_udp_socket(port_t local_port_ho, ipv4_addr_t local_ip_ho);

/* Socket management (used by tcp.c) */
int net_alloc_socket_slot(void);
net_socket_t *net_get_socket(int fd);
void net_free_socket_slot(int fd);
uint32_t net_get_tcp_initial_seq(void);

/* RX buffer (used by tcp.c, udp.c) */
int net_rx_buffer_write(net_socket_t *s, const void *data, size_t len);
int net_rx_buffer_read(net_socket_t *s, void *data, size_t len);

/* Initialization */
void net_init(void);

#endif
