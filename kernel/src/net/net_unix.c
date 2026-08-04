/*
 * net_unix.c - AF_UNIX (Unix domain socket) implementation for SpiritFoxOS
 *
 * Provides local inter-process communication via filesystem pathnames.
 * Critical for Wayland compositor communication.
 */

#include "net.h"
#include "net_socket.h"
#include "net_unix.h"
#include "kmalloc.h"
#include "string.h"
#include "serial.h"
#include "hal.h"
#include "process.h"
#include "smp.h"
#include "errno.h"

#define UNIX_MAX_LISTENERS 32

/* Global table of bound Unix sockets (path -> fd) */
static struct {
    char path[UNIX_PATH_MAX];
    int  fd;
} unix_listeners[UNIX_MAX_LISTENERS];
static int unix_listener_count = 0;

/* Find a listening Unix socket by path, return fd or -1 */
static int unix_find_listener(const char *path)
{
    for (int i = 0; i < unix_listener_count; i++) {
        if (strcmp(unix_listeners[i].path, path) == 0)
            return unix_listeners[i].fd;
    }
    return -1;
}

/* Register a Unix socket listener */
static int unix_register_listener(const char *path, int fd)
{
    if (unix_listener_count >= UNIX_MAX_LISTENERS)
        return -ENOMEM;
    strcpy(unix_listeners[unix_listener_count].path, path);
    unix_listeners[unix_listener_count].fd = fd;
    unix_listener_count++;
    return 0;
}

/* Unregister a Unix socket listener */
static void unix_unregister_listener(const char *path)
{
    for (int i = 0; i < unix_listener_count; i++) {
        if (strcmp(unix_listeners[i].path, path) == 0) {
            unix_listeners[i] = unix_listeners[unix_listener_count - 1];
            unix_listener_count--;
            return;
        }
    }
}

/* Create an AF_UNIX socket */
int unix_socket(int type, int protocol)
{
    /* Strip SOCK_NONBLOCK (0x80000) and SOCK_CLOEXEC (0x80000 on some, 0x80000000 on Linux)
     * Linux uses SOCK_CLOEXEC=0x80000, SOCK_NONBLOCK=0x800 */
    int base_type = type & 0xFF;  /* SOCK_STREAM=1, SOCK_DGRAM=2, SOCK_SEQPACKET=5 */
    if (base_type != SOCK_STREAM)
        return -ESOCKTNOSUPPORT;
    if (protocol != 0)
        return -EPROTONOSUPPORT;

    int fd = net_alloc_socket_slot();
    if (fd < 0)
        return -ENOMEM;

    net_socket_t *s = net_get_socket(fd);
    if (!s) {
        net_free_socket_slot(fd);
        return -ENOMEM;
    }

    memset(s, 0, sizeof(*s));
    s->fd = fd;
    s->domain = AF_UNIX;
    s->type = SOCK_STREAM;
    s->protocol = 0;
    s->state = TCP_CLOSED;  /* reuse TCP states: CLOSED -> LISTEN -> ESTABLISHED */
    s->flags = 0;
    if (type & 0x80000)  /* SOCK_CLOEXEC */
        s->flags |= SOCKF_CLOEXEC;
    if (type & 0x800)    /* SOCK_NONBLOCK */
        s->flags |= SOCKF_NONBLOCK;

    /* Allocate rx buffer */
    s->rx_size = NET_RX_BUF_SIZE;
    s->rx_buf = kmalloc(s->rx_size);
    if (!s->rx_buf) {
        net_free_socket_slot(fd);
        return -ENOMEM;
    }
    s->rx_read_pos = 0;
    s->rx_write_pos = 0;
    s->rx_count = 0;

    s->unix_path[0] = '\0';
    s->unix_peer = NULL;
    s->blocking_pid = -1;

    return fd;
}

/* Bind an AF_UNIX socket to a path */
int unix_bind(int fd, const sockaddr_un_t *addr, socklen_t addrlen)
{
    net_socket_t *s = net_get_socket(fd);
    if (!s || s->domain != AF_UNIX)
        return -EBADF;
    if (!addr || addrlen < (socklen_t)offsetof(sockaddr_un_t, sun_path) + 1)
        return -EINVAL;
    if (s->unix_path[0] != '\0')
        return -EINVAL;  /* already bound */

    /* Check if path already in use */
    if (unix_find_listener(addr->sun_path) >= 0)
        return -EADDRINUSE;

    strncpy(s->unix_path, addr->sun_path, UNIX_PATH_MAX - 1);
    s->unix_path[UNIX_PATH_MAX - 1] = '\0';

    return 0;
}

/* Listen for connections on an AF_UNIX socket */
int unix_listen(int fd, int backlog)
{
    (void)backlog;
    net_socket_t *s = net_get_socket(fd);
    if (!s || s->domain != AF_UNIX)
        return -EBADF;
    if (s->unix_path[0] == '\0')
        return -EINVAL;  /* not bound */

    s->state = TCP_LISTEN;
    s->pending_count = 0;

    /* Register as a listener */
    int ret = unix_register_listener(s->unix_path, fd);
    if (ret < 0)
        return ret;

    serial_puts("[UNIX] listening on ");
    serial_puts(s->unix_path);
    serial_puts(" (fd=");
    serial_put_dec((uint64_t)fd);
    serial_puts(")\n");
    return 0;
}

/* Accept a connection on an AF_UNIX socket */
int unix_accept(int fd, sockaddr_un_t *addr, socklen_t *addrlen)
{
    net_socket_t *s = net_get_socket(fd);
    if (!s || s->domain != AF_UNIX)
        return -EBADF;
    if (s->state != TCP_LISTEN)
        return -EINVAL;

    /* Check for pending connections */
    if (s->pending_count == 0) {
        if (s->flags & SOCKF_NONBLOCK)
            return -EAGAIN;

        /* Block until a connection arrives */
        process_t *cur = process_current();
        if (cur) {
            s->blocking_pid = cur->pid;
            cur->state = PROC_BLOCKED;
            this_cpu()->need_reschedule = 1;
            scheduler_schedule();
        }

        /* Woken up -- check again */
        if (s->pending_count == 0)
            return -EAGAIN;
    }

    /* Dequeue the first pending connection */
    net_socket_t *client = s->pending_conns[0];
    for (int i = 1; i < s->pending_count; i++)
        s->pending_conns[i - 1] = s->pending_conns[i];
    s->pending_count--;

    /* Create a new server-side socket for this connection */
    int new_fd = net_alloc_socket_slot();
    if (new_fd < 0)
        return -ENOMEM;

    net_socket_t *server_conn = net_get_socket(new_fd);
    if (!server_conn) {
        net_free_socket_slot(new_fd);
        return -ENOMEM;
    }

    memset(server_conn, 0, sizeof(*server_conn));
    server_conn->fd = new_fd;
    server_conn->domain = AF_UNIX;
    server_conn->type = SOCK_STREAM;
    server_conn->state = TCP_ESTABLISHED;

    /* Allocate rx buffer */
    server_conn->rx_size = NET_RX_BUF_SIZE;
    server_conn->rx_buf = kmalloc(server_conn->rx_size);
    if (!server_conn->rx_buf) {
        net_free_socket_slot(new_fd);
        return -ENOMEM;
    }

    /* Link the two sockets as peers */
    server_conn->unix_peer = client;
    client->unix_peer = server_conn;
    client->state = TCP_ESTABLISHED;

    /* If client was blocked waiting for connect, wake it */
    if (client->blocking_pid >= 0) {
        process_t *proc = process_get(client->blocking_pid);
        if (proc && proc->state == PROC_BLOCKED) {
            proc->state = PROC_READY;
            client->blocking_pid = -1;
        }
    }

    /* Return address info */
    if (addr && addrlen) {
        addr->sun_family = AF_UNIX;
        strncpy(addr->sun_path, client->unix_path, UNIX_PATH_MAX - 1);
        *addrlen = (socklen_t)(offsetof(sockaddr_un_t, sun_path) + strlen(addr->sun_path) + 1);
    }

    serial_puts("[UNIX] accepted connection on ");
    serial_puts(s->unix_path);
    serial_puts(" (new_fd=");
    serial_put_dec((uint64_t)new_fd);
    serial_puts(")\n");
    return new_fd;
}

/* Connect to an AF_UNIX socket */
int unix_connect(int fd, const sockaddr_un_t *addr, socklen_t addrlen)
{
    (void)addrlen;
    net_socket_t *s = net_get_socket(fd);
    if (!s || s->domain != AF_UNIX)
        return -EBADF;
    if (!addr)
        return -EINVAL;

    /* Find the listening socket */
    int listen_fd = unix_find_listener(addr->sun_path);
    if (listen_fd < 0)
        return -ECONNREFUSED;

    net_socket_t *listener = net_get_socket(listen_fd);
    if (!listener || listener->state != TCP_LISTEN)
        return -ECONNREFUSED;

    if (listener->pending_count >= NET_TCP_BACKLOG)
        return -ECONNREFUSED;

    /* Set our path (client path, for identification) */
    strncpy(s->unix_path, addr->sun_path, UNIX_PATH_MAX - 1);
    s->unix_path[UNIX_PATH_MAX - 1] = '\0';

    /* Add ourselves to the listener's pending connection queue */
    listener->pending_conns[listener->pending_count++] = s;
    s->state = TCP_SYN_SENT;  /* connecting */

    /* Wake up the listener if it was blocked in accept() */
    if (listener->blocking_pid >= 0) {
        process_t *proc = process_get(listener->blocking_pid);
        if (proc && proc->state == PROC_BLOCKED) {
            proc->state = PROC_READY;
            listener->blocking_pid = -1;
        }
    }

    /* Block until the connection is accepted */
    if (s->state != TCP_ESTABLISHED) {
        process_t *cur = process_current();
        if (cur) {
            s->blocking_pid = cur->pid;
            cur->state = PROC_BLOCKED;
            this_cpu()->need_reschedule = 1;
            scheduler_schedule();
        }
    }

    serial_puts("[UNIX] connected to ");
    serial_puts(addr->sun_path);
    serial_puts(" (fd=");
    serial_put_dec((uint64_t)fd);
    serial_puts(")\n");
    return 0;
}

/* Send data on an AF_UNIX socket */
int unix_send(int fd, const void *buf, size_t len, int flags)
{
    (void)flags;
    net_socket_t *s = net_get_socket(fd);
    if (!s || s->domain != AF_UNIX)
        return -EBADF;
    if (s->state != TCP_ESTABLISHED)
        return -ENOTCONN;

    net_socket_t *peer = s->unix_peer;
    if (!peer)
        return -EPIPE;

    /* Write data to peer's rx buffer */
    int written = net_rx_buffer_write(peer, buf, len);
    if (written < 0)
        return -EAGAIN;

    /* Wake up any process blocked on recv for the peer */
    if (peer->blocking_pid >= 0) {
        process_t *proc = process_get(peer->blocking_pid);
        if (proc && proc->state == PROC_BLOCKED) {
            proc->state = PROC_READY;
            peer->blocking_pid = -1;
        }
    }

    return written;
}

/* Receive data on an AF_UNIX socket */
int unix_recv(int fd, void *buf, size_t len, int flags)
{
    (void)flags;
    net_socket_t *s = net_get_socket(fd);
    if (!s || s->domain != AF_UNIX)
        return -EBADF;
    if (s->state != TCP_ESTABLISHED)
        return -ENOTCONN;

    /* Check if data is available */
    if (s->rx_count == 0) {
        if (s->flags & SOCKF_NONBLOCK)
            return -EAGAIN;

        /* Block until data arrives */
        process_t *cur = process_current();
        if (cur) {
            s->blocking_pid = cur->pid;
            cur->state = PROC_BLOCKED;
            this_cpu()->need_reschedule = 1;
            scheduler_schedule();
        }
    }

    int rd = net_rx_buffer_read(s, buf, len);
    return rd;
}

/* Close an AF_UNIX socket */
int unix_close(int fd)
{
    net_socket_t *s = net_get_socket(fd);
    if (!s || s->domain != AF_UNIX)
        return -EBADF;

    /* If bound, unregister from listener table */
    if (s->unix_path[0] != '\0' && s->state == TCP_LISTEN)
        unix_unregister_listener(s->unix_path);

    /* Disconnect peer */
    if (s->unix_peer) {
        s->unix_peer->unix_peer = NULL;
        s->unix_peer->state = TCP_CLOSE_WAIT;
        /* Wake up peer if blocked */
        if (s->unix_peer->blocking_pid >= 0) {
            process_t *proc = process_get(s->unix_peer->blocking_pid);
            if (proc && proc->state == PROC_BLOCKED) {
                proc->state = PROC_READY;
                s->unix_peer->blocking_pid = -1;
            }
        }
    }

    /* Free rx buffer */
    if (s->rx_buf) {
        kfree(s->rx_buf);
        s->rx_buf = NULL;
    }

    /* Free tx buffer */
    if (s->tx_buf) {
        kfree(s->tx_buf);
        s->tx_buf = NULL;
    }

    net_free_socket_slot(fd);
    return 0;
}
