#include "syscall_internal.h"
#include "process.h"
#include "net.h"
#include "net_socket.h"
#include "errno.h"

/* ========================================================================
 * 网络系统调用
 * ======================================================================== */

int64_t sys_socket(trap_frame_t *frame)
{
    int domain   = (int)frame->rdi;
    int type     = (int)frame->rsi;
    int protocol = (int)frame->rdx;
    return net_socket(domain, type, protocol);
}

int64_t sys_connect(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    const net_sockaddr_t *addr = (const net_sockaddr_t *)frame->rsi;
    socklen_t addrlen = (socklen_t)frame->rdx;
    return net_connect(fd, addr, addrlen);
}

int64_t sys_accept(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    net_sockaddr_t *addr = (net_sockaddr_t *)frame->rsi;
    socklen_t *addrlen = (socklen_t *)frame->rdx;
    return net_accept(fd, addr, addrlen);
}

int64_t sys_bind(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    const net_sockaddr_t *addr = (const net_sockaddr_t *)frame->rsi;
    socklen_t addrlen = (socklen_t)frame->rdx;
    return net_bind(fd, addr, addrlen);
}

int64_t sys_listen(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    int backlog = (int)frame->rsi;
    return net_listen(fd, backlog);
}

int64_t sys_sendto(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    const void *buf = (const void *)frame->rsi;
    size_t len = (size_t)frame->rdx;
    int flags = (int)frame->r10;
    const net_sockaddr_t *dest_addr = (const net_sockaddr_t *)frame->r8;
    socklen_t addrlen = (socklen_t)frame->r9;
    if (dest_addr)
        return net_sendto(fd, buf, len, flags, dest_addr, addrlen);
    return net_send(fd, buf, len, flags);
}

int64_t sys_recvfrom(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    void *buf = (void *)frame->rsi;
    size_t len = (size_t)frame->rdx;
    int flags = (int)frame->r10;
    net_sockaddr_t *src_addr = (net_sockaddr_t *)frame->r8;
    socklen_t *addrlen = (socklen_t *)frame->r9;
    if (src_addr)
        return net_recvfrom(fd, buf, len, flags, src_addr, addrlen);
    return net_recv(fd, buf, len, flags);
}

int64_t sys_shutdown(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    int how = (int)frame->rsi;
    return net_shutdown(fd, how);
}

int64_t sys_getsockname(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    net_sockaddr_t *addr = (net_sockaddr_t *)frame->rsi;
    socklen_t *addrlen = (socklen_t *)frame->rdx;
    return net_getsockname(fd, addr, addrlen);
}

int64_t sys_getpeername(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    net_sockaddr_t *addr = (net_sockaddr_t *)frame->rsi;
    socklen_t *addrlen = (socklen_t *)frame->rdx;
    return net_getpeername(fd, addr, addrlen);
}

int64_t sys_socketpair(trap_frame_t *frame)
{
    (void)frame;
    return -EAFNOSUPPORT;
}

int64_t sys_setsockopt(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    int level = (int)frame->rsi;
    int optname = (int)frame->rdx;
    const void *optval = (const void *)frame->r10;
    socklen_t optlen = (socklen_t)frame->r8;
    return net_setsockopt(fd, level, optname, optval, optlen);
}

int64_t sys_getsockopt(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    int level = (int)frame->rsi;
    int optname = (int)frame->rdx;
    void *optval = (void *)frame->r10;
    socklen_t *optlen = (socklen_t *)frame->r8;
    return net_getsockopt(fd, level, optname, optval, optlen);
}
