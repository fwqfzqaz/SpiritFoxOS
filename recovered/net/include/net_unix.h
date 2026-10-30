#ifndef NET_UNIX_H
#define NET_UNIX_H

#include "net.h"
#include "net_socket.h"

int unix_socket(int type, int protocol);
int unix_bind(int fd, const sockaddr_un_t *addr, socklen_t addrlen);
int unix_listen(int fd, int backlog);
int unix_accept(int fd, sockaddr_un_t *addr, socklen_t *addrlen);
int unix_connect(int fd, const sockaddr_un_t *addr, socklen_t addrlen);
int unix_send(int fd, const void *buf, size_t len, int flags,
              const int *fds, int fd_count);
int unix_recv(int fd, void *buf, size_t len, int flags,
              int *fds, int *fd_count);
int unix_close(int fd);
void unix_unregister_listener(const char *path);

#endif /* NET_UNIX_H */
