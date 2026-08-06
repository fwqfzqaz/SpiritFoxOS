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
#include "vfs.h"
#include "memory.h"

#define UNIX_MAX_LISTENERS 32

/* Global table of bound Unix sockets (path -> socket pointer) */
static struct {
    char path[UNIX_PATH_MAX];
    net_socket_t *sock;
} unix_listeners[UNIX_MAX_LISTENERS];
static int unix_listener_count = 0;

/* Find a listening Unix socket by path, return socket pointer or NULL */
static net_socket_t *unix_find_listener(const char *path)
{
    for (int i = 0; i < unix_listener_count; i++) {
        if (strcmp(unix_listeners[i].path, path) == 0)
            return unix_listeners[i].sock;
    }
    return NULL;
}

/* Register a Unix socket listener */
static int unix_register_listener(const char *path, net_socket_t *sock)
{
    if (unix_listener_count >= UNIX_MAX_LISTENERS)
        return -ENOMEM;
    strcpy(unix_listeners[unix_listener_count].path, path);
    unix_listeners[unix_listener_count].sock = sock;
    unix_listener_count++;
    return 0;
}

/* Unregister a Unix socket listener */
void unix_unregister_listener(const char *path)
{
    for (int i = 0; i < unix_listener_count; i++) {
        if (strcmp(unix_listeners[i].path, path) == 0) {
            unix_listeners[i] = unix_listeners[unix_listener_count - 1];
            unix_listener_count--;
            return;
        }
    }
}

/* Release a reference on a duplicated vfs_file that was queued for
 * SCM_RIGHTS delivery but will not be consumed (e.g. socket closed or
 * the receiver's fd table / cmsg buffer is full).  Uses the standard
 * close path so pipe / chardev cleanup is handled consistently. */
static void unix_release_file(vfs_file_t *f)
{
    if (!f)
        return;

    int tmpfd = vfs_alloc_fd();
    if (tmpfd >= 0) {
        vfs_get_fd_table()[tmpfd] = f;
        vfs_close(tmpfd);
        return;
    }

    /* No free fd slot available: drop the reference directly. */
    f->refcount--;
    if (f->refcount <= 0)
        free_page(f);
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

    int net_fd = net_alloc_socket_slot();
    if (net_fd < 0)
        return -ENOMEM;

    net_socket_t *s = net_get_socket(net_fd);
    if (!s) {
        net_free_socket_slot(net_fd);
        return -ENOMEM;
    }

    memset(s, 0, sizeof(*s));
    s->fd = net_fd;  /* temporarily the net slot index */
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
        net_free_socket_slot(net_fd);
        return -ENOMEM;
    }
    s->rx_read_pos = 0;
    s->rx_write_pos = 0;
    s->rx_count = 0;

    s->unix_path[0] = '\0';
    s->unix_peer = NULL;
    s->blocking_pid = -1;

    /* Register in VFS fd table to unify the fd namespace */
    int vfs_fd = vfs_register_socket(s);
    if (vfs_fd < 0) {
        net_free_socket_slot(net_fd);
        kfree(s->rx_buf);
        return vfs_fd;
    }

    return vfs_fd;
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
    if (unix_find_listener(addr->sun_path) != NULL)
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

    /* Register as a listener — store the socket pointer directly
     * so that cross-process lookups work regardless of fd namespace */
    int ret = unix_register_listener(s->unix_path, s);
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
    int net_fd = net_alloc_socket_slot();
    if (net_fd < 0)
        return -ENOMEM;

    net_socket_t *server_conn = net_get_socket(net_fd); /* gets from net slot table */
    if (!server_conn) {
        net_free_socket_slot(net_fd);
        return -ENOMEM;
    }

    memset(server_conn, 0, sizeof(*server_conn));
    server_conn->fd = net_fd;  /* temporarily net slot, vfs_register_socket updates it */
    server_conn->domain = AF_UNIX;
    server_conn->type = SOCK_STREAM;
    server_conn->state = TCP_ESTABLISHED;
    /* Accepted sockets are non-blocking by default — libwayland-server
     * expects this and does not explicitly set O_NONBLOCK.  Without it,
     * recvmsg blocks when no data is available, freezing the compositor. */
    server_conn->flags |= 0x01;  /* SOCKF_NONBLOCK */

    /* Allocate rx buffer */
    server_conn->rx_size = NET_RX_BUF_SIZE;
    server_conn->rx_buf = kmalloc(server_conn->rx_size);
    if (!server_conn->rx_buf) {
        net_free_socket_slot(net_fd);
        return -ENOMEM;
    }

    /* Link the two sockets as peers */
    server_conn->unix_peer = client;
    client->unix_peer = server_conn;
    client->state = TCP_ESTABLISHED;

    /* Set SO_PEERCRED on the server-side socket:
     * the peer of the server socket is the client process */
    server_conn->peer_pid = client->peer_pid;
    server_conn->peer_uid = client->peer_uid;
    server_conn->peer_gid = client->peer_gid;

    /* If client was blocked waiting for connect, wake it */
    if (client->blocking_pid >= 0) {
        process_t *proc = process_get(client->blocking_pid);
        if (proc && proc->state == PROC_BLOCKED) {
            process_wake(proc);
            client->blocking_pid = -1;
        }
    }

    /* Register the new server-side socket in VFS fd table */
    int vfs_fd = vfs_register_socket(server_conn);
    if (vfs_fd < 0) {
        kfree(server_conn->rx_buf);
        net_free_socket_slot(net_fd);
        return vfs_fd;
    }

    /* Set O_NONBLOCK on the VFS fd as well — the socket flags alone
     * are not enough for fcntl(F_GETFL) to report non-blocking. */
    {
        vfs_file_t **fds = vfs_get_fd_table();
        if (fds && fds[vfs_fd])
            fds[vfs_fd]->flags |= 0x800;  /* LINUX_O_NONBLOCK */
    }

    /* Return address info - skip if userspace pointer is inaccessible.
     * libwayland passes NULL for addr, so this rarely matters.
     * TODO: implement copy_to_user for proper userspace writes. */
    (void)addr;
    (void)addrlen;

    serial_puts("[UNIX] accepted connection on ");
    serial_puts(s->unix_path);
    serial_puts(" (new_vfs_fd=");
    serial_put_dec((uint64_t)vfs_fd);
    serial_puts(")\n");
    return vfs_fd;
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

    /* Find the listening socket — returns the socket pointer directly,
     * bypassing the fd namespace so cross-process lookups work */
    net_socket_t *listener = unix_find_listener(addr->sun_path);
    if (!listener)
        return -ECONNREFUSED;

    if (listener->state != TCP_LISTEN)
        return -ECONNREFUSED;

    if (listener->pending_count >= NET_TCP_BACKLOG)
        return -ECONNREFUSED;

    /* Set our path (client path, for identification) */
    strncpy(s->unix_path, addr->sun_path, UNIX_PATH_MAX - 1);
    s->unix_path[UNIX_PATH_MAX - 1] = '\0';

    /* Add ourselves to the listener's pending connection queue */
    listener->pending_conns[listener->pending_count++] = s;
    s->state = TCP_SYN_SENT;  /* connecting */

    /* Record the connecting process credentials for SO_PEERCRED */
    {
        process_t *cur = process_current();
        if (cur) {
            s->peer_pid = cur->pid;
            s->peer_uid = 0;  /* root for now */
            s->peer_gid = 0;  /* root for now */
        }
    }

    /* Wake up the listener if it was blocked in accept() */
    if (listener->blocking_pid >= 0) {
        process_t *proc = process_get(listener->blocking_pid);
        if (proc && proc->state == PROC_BLOCKED) {
            process_wake(proc);
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

/* Send data on an AF_UNIX socket.  If fds != NULL and fd_count > 0, the
 * referenced file descriptors are passed to the peer via SCM_RIGHTS:
 * each vfs_file is duplicated (refcount bumped) and queued on the peer's
 * pending list, to be materialized into the receiver's fd table by
 * unix_recv/recvmsg. */
int unix_send(int fd, const void *buf, size_t len, int flags,
              const int *fds, int fd_count)
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

    /* Validate fds to pass via SCM_RIGHTS up front (before sending). */
    if (fd_count > 0) {
        if (!fds || fd_count > 8)
            return -EINVAL;
        if (peer->pending_fd_count + fd_count > 8)
            return -EINVAL;  /* no room in peer's pending fd buffer */
        vfs_file_t **sender_fds = vfs_get_fd_table();
        for (int i = 0; i < fd_count; i++) {
            int f = fds[i];
            if (f < 0 || f >= VFS_MAX_FD || !sender_fds[f])
                return -EBADF;
        }
    }

    /* Write data to peer's rx buffer */
    int written = net_rx_buffer_write(peer, buf, len);
    if (written < 0)
        return -EAGAIN;

    /* Queue passed fds on the peer: duplicate each (bump refcount) and
     * hold the reference in pending_files until the peer consumes it. */
    if (fd_count > 0) {
        int base = peer->pending_fd_count;
        for (int i = 0; i < fd_count; i++) {
            int tmpfd = vfs_dup(fds[i]);
            if (tmpfd < 0) {
                /* Rollback fds queued so far in this call */
                for (int j = 0; j < i; j++)
                    unix_release_file(peer->pending_files[base + j]);
                peer->pending_fd_count = base;
                return -EBADF;
            }
            /* Detach the temporary sender fd slot but keep the refcount
             * bump; the reference now belongs to pending_files. */
            vfs_file_t *f = vfs_get_fd_table()[tmpfd];
            vfs_get_fd_table()[tmpfd] = NULL;
            peer->pending_files[base + i] = f;
            peer->pending_fd_count = base + i + 1;
        }
    }

    /* Wake up any process blocked on recv for the peer */
    if (peer->blocking_pid >= 0) {
        process_t *proc = process_get(peer->blocking_pid);
        if (proc && proc->state == PROC_BLOCKED) {
            process_wake(proc);
            peer->blocking_pid = -1;
        }
    }

    return written;
}

/* Receive data on an AF_UNIX socket.  If fds/fd_count are non-NULL, any
 * fds previously passed via SCM_RIGHTS are materialized into the current
 * (receiver) process's fd table and reported through fds[]; *fd_count is
 * the caller capacity on input and the number delivered on output.  Undelivered
 * pending fds (capacity exceeded) are released so they do not leak. */
int unix_recv(int fd, void *buf, size_t len, int flags,
              int *fds, int *fd_count)
{
    net_socket_t *s = net_get_socket(fd);
    if (!s || s->domain != AF_UNIX)
        return -EBADF;
    if (s->state != TCP_ESTABLISHED)
        return -ENOTCONN;

    int dontwait = (flags & MSG_DONTWAIT) || (s->flags & SOCKF_NONBLOCK);

    {
        static int recv_log_count = 0;
        recv_log_count++;
        if (recv_log_count <= 50 || recv_log_count % 1000 == 0) {
            serial_puts("[unix_recv] fd=");
            serial_put_dec((uint64_t)fd);
            serial_puts(" flags=0x");
            serial_put_hex((uint64_t)flags);
            serial_puts(" dontwait=");
            serial_put_dec((uint64_t)dontwait);
            serial_puts(" rx_count=");
            serial_put_dec((uint64_t)s->rx_count);
            serial_puts("\n");
        }
    }

    int has_fd_request = (fds != NULL && fd_count != NULL);

    /* Block until data or passed fds are available.  A recvmsg that only
     * needs to drain pending fds must not block on absent data. */
    if (s->rx_count == 0 && (!has_fd_request || s->pending_fd_count == 0)) {
        if (dontwait)
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

    /* Materialize any pending SCM_RIGHTS fds into the receiver's table. */
    if (has_fd_request) {
        int capacity = *fd_count;
        if (capacity < 0)
            capacity = 0;
        if (capacity > 8)
            capacity = 8;
        int n = s->pending_fd_count;
        int delivered = 0;

        if (n > 0 && capacity > 0) {
            vfs_file_t **recv_fds = vfs_get_fd_table();
            for (int i = 0; i < n && i < capacity; i++) {
                int newfd = vfs_alloc_fd();
                if (newfd < 0)
                    break;  /* receiver fd table full */
                recv_fds[newfd] = s->pending_files[i];
                s->pending_fds[i] = newfd;
                fds[i] = newfd;
                delivered++;
            }
        }

        /* Release any pending fds that could not be delivered. */
        for (int i = delivered; i < n; i++)
            unix_release_file(s->pending_files[i]);

        *fd_count = delivered;

        /* Reset the pending queues (consumed). */
        for (int i = 0; i < s->pending_fd_count; i++) {
            s->pending_files[i] = NULL;
            s->pending_fds[i] = 0;
        }
        s->pending_fd_count = 0;
    }

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
                process_wake(proc);
                s->unix_peer->blocking_pid = -1;
            }
        }
    }

    /* Release any unconsumed SCM_RIGHTS file references */
    for (int i = 0; i < s->pending_fd_count; i++) {
        if (s->pending_files[i])
            unix_release_file(s->pending_files[i]);
        s->pending_files[i] = NULL;
        s->pending_fds[i] = 0;
    }
    s->pending_fd_count = 0;

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

    /* Get the net slot index from the VFS inode's fs_data */
    vfs_file_t **fds = vfs_get_fd_table();
    int net_fd = -1;
    if (fds && fd >= 0 && fd < VFS_MAX_FD && fds[fd] && fds[fd]->inode)
        net_fd = (int)(uintptr_t)fds[fd]->inode->fs_data;

    if (net_fd >= 0)
        net_free_socket_slot(net_fd);

    return 0;
}
