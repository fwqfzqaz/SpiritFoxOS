#include "syscall_internal.h"
#include "process.h"
#include "vfs.h"
#include "hal.h"
#include "mmu.h"
#include "string.h"
#include "registry.h"
#include "timer.h"
#include "net.h"
#include "net_socket.h"
#include "errno.h"
#include "futex.h"
#include "sfk_perms.h"
#include "serial.h"
#include "kmalloc.h"
#include "chardev.h"

/* 时钟 ID */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2

/* 前向声明：以下类型在 epoll_check_fd 中需要引用，提前定义 */
typedef struct {
    uint64_t counter;
    int      flags;
} eventfd_instance_t;

typedef struct {
    int        clockid;
    int        flags;
    uint64_t   initial_ms;
    uint64_t   interval_ms;
    uint64_t   next_expiry;
    int        armed;
} timerfd_instance_t;

/* timespec structure */
typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} linux_timespec_t;

/* timeval 结构体 */
typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
} linux_timeval_t;

/* Linux 兼容的 utsname 结构体 */
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
} linux_utsname_t;

/* ========================================================================
 * Misc syscalls
 * ======================================================================== */

int64_t sys_uname(trap_frame_t *frame)
{
    linux_utsname_t *buf = (linux_utsname_t *)frame->rdi;
    if (!buf)
        return -EFAULT;

    /* 填充 Linux 兼容的 utsname */
    const char *sysname = "Linux";
    const char *nodename = "SpiritFoxOS";
    const char *release = "6.1.0-sfk";
    const char *version = "#1 SMP SpiritFoxOS";
    const char *machine = "x86_64";

    for (int i = 0; i < 65; i++) {
        buf->sysname[i]  = sysname[i];
        buf->nodename[i] = nodename[i];
        buf->release[i]  = release[i];
        buf->version[i]  = version[i];
        buf->machine[i]  = machine[i];
        if (!sysname[i] && !nodename[i] && !release[i] && !version[i] && !machine[i])
            break;
    }

    return 0;
}

int64_t sys_gettimeofday(trap_frame_t *frame)
{
    linux_timeval_t *tv = (linux_timeval_t *)frame->rdi;
    (void)tv;
    /* Stub: return 0, zero timeval */
    if (tv) {
        tv->tv_sec = 0;
        tv->tv_usec = 0;
    }
    return 0;
}

int64_t sys_clock_gettime(trap_frame_t *frame)
{
    int clk_id = (int)frame->rdi;
    linux_timespec_t *tp = (linux_timespec_t *)frame->rsi;

    if (!tp)
        return -EFAULT;

    uint64_t ms = timer_get_ms();

    switch (clk_id) {
    case CLOCK_REALTIME:
    case CLOCK_MONOTONIC:
        tp->tv_sec  = (int64_t)(ms / 1000);
        tp->tv_nsec = (int64_t)((ms % 1000) * 1000000);
        return 0;
    case CLOCK_PROCESS_CPUTIME_ID:
        if (process_current()) {
            tp->tv_sec  = (int64_t)(process_current()->cpu_time / 1000);
            tp->tv_nsec = (int64_t)((process_current()->cpu_time % 1000) * 1000000);
        }
        return 0;
    default:
        return -EINVAL;
    }
}

int64_t sys_nanosleep(trap_frame_t *frame)
{
    const linux_timespec_t *req = (const linux_timespec_t *)frame->rdi;
    linux_timespec_t *rem = (linux_timespec_t *)frame->rsi;

    if (!req)
        return -EFAULT;

    uint64_t ms = (uint64_t)req->tv_sec * 1000 +
                  (uint64_t)req->tv_nsec / 1000000;
    if (ms > 0)
        process_sleep(ms);

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

int64_t sys_time(trap_frame_t *frame)
{
    int64_t *tloc = (int64_t *)frame->rdi;
    int64_t t = (int64_t)(timer_get_ms() / 1000);
    if (tloc)
        *tloc = t;
    return t;
}

int64_t sys_sched_yield(trap_frame_t *frame)
{
    (void)frame;
    process_yield();
    return 0;
}

/* Linux pollfd 结构 */
typedef unsigned long nfds_t;

struct linux_pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN      0x0001
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

/* EPOLLIN/POLLIN, EPOLLOUT/POLLOUT 等值相同，可互换使用 */
static uint32_t epoll_check_fd(int fd);

int64_t sys_poll(trap_frame_t *frame)
{
    struct linux_pollfd *fds = (struct linux_pollfd *)frame->rdi;
    nfds_t nfds = (nfds_t)frame->rsi;
    int timeout = (int)frame->rdx;

    if (!fds && nfds > 0)
        return -EFAULT;

    process_t *proc = process_current();
    if (!proc)
        return -ESRCH;

    /* 在内核空间构建 pollfd 副本，避免每次循环都读用户空间 */
    struct linux_pollfd *kfds = NULL;
    if (nfds > 0) {
        kfds = kmalloc(sizeof(struct linux_pollfd) * nfds);
        if (!kfds)
            return -ENOMEM;
        uint64_t phys = mmu_virt_to_phys(proc->pml4, (uint64_t)fds);
        if (!phys) {
            kfree(kfds);
            return -EFAULT;
        }
        memcpy(kfds, (void *)(uintptr_t)phys, sizeof(struct linux_pollfd) * nfds);
        /* 清零 revents */
        for (nfds_t i = 0; i < nfds; i++)
            kfds[i].revents = 0;
    }

    int total_revents = 0;
    int elapsed_ms = 0;

    for (;;) {
        total_revents = 0;
        for (nfds_t i = 0; i < nfds; i++) {
            short ev = kfds[i].events;
            kfds[i].revents = 0;

            if (kfds[i].fd < 0)
                continue;

            /* 复用 epoll_check_fd 逻辑获取事件状态 */
            uint32_t ep_events = epoll_check_fd(kfds[i].fd);

            if ((ev & POLLIN) && (ep_events & POLLIN))
                kfds[i].revents |= POLLIN;
            if ((ev & POLLOUT) && (ep_events & POLLOUT))
                kfds[i].revents |= POLLOUT;
            /* POLLERR 和 POLLHUP 始终报告 */
            if (ep_events & POLLERR)
                kfds[i].revents |= POLLERR;
            if (ep_events & POLLHUP)
                kfds[i].revents |= POLLHUP;

            /* 检查 fd 是否有效 */
            vfs_file_t **fd_table = process_get_fd_table();
            if (!fd_table || kfds[i].fd >= PROC_MAX_FD || !fd_table[kfds[i].fd]) {
                vfs_file_t **vfs_fds = vfs_get_fd_table();
                if (!vfs_fds || kfds[i].fd >= VFS_MAX_FD || !vfs_fds[kfds[i].fd])
                    kfds[i].revents |= POLLNVAL;
            }

            if (kfds[i].revents)
                total_revents++;
        }

        if (total_revents > 0)
            break;

        if (timeout == 0)
            break;

        if (timeout > 0 && elapsed_ms >= timeout)
            break;

        /* 对于有 Unix socket 的 poll，在休眠前注册 blocking_pid
         * 这样当对端发送数据时 unix_send() 能通过 process_wake() 立即唤醒。
         * 只在 blocking_pid 未被注册时设置，避免覆盖其他等待者。 */
        for (nfds_t i = 0; i < nfds; i++) {
            if (kfds[i].fd < 0)
                continue;
            net_socket_t *sock = net_get_socket(kfds[i].fd);
            if (sock && sock->domain == AF_UNIX &&
                sock->state == TCP_ESTABLISHED &&
                sock->blocking_pid < 0 &&
                (kfds[i].events & POLLIN)) {
                sock->blocking_pid = proc->pid;
            }
        }

        /* 使用较长的休眠时间，依赖 process_wake() 在数据到达时立即唤醒。
         * 之前 process_sleep(1) 导致每 1ms 轮询一次，浪费 CPU。
         * 100ms 足够长以减少轮询开销，同时又保证超时精度。 */
        int sleep_ms = 100;
        if (timeout > 0) {
            int remaining = timeout - elapsed_ms;
            if (remaining < sleep_ms)
                sleep_ms = remaining;
        }
        process_sleep(sleep_ms);
        elapsed_ms += sleep_ms;
    }

    /* 清除注册的 blocking_pid */
    for (nfds_t i = 0; i < nfds; i++) {
        if (kfds[i].fd < 0)
            continue;
        net_socket_t *sock = net_get_socket(kfds[i].fd);
        if (sock && sock->blocking_pid == proc->pid)
            sock->blocking_pid = -1;
    }

    /* 将结果写回用户空间 */
    if (nfds > 0 && kfds) {
        for (nfds_t i = 0; i < nfds; i++) {
            uint64_t phys = mmu_virt_to_phys(proc->pml4,
                              (uint64_t)&fds[i].revents);
            if (phys)
                *(short *)(uintptr_t)phys = kfds[i].revents;
        }
        kfree(kfds);
    }

    return total_revents;
}

/* ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo,
 *       const sigset_t *sigmask)
 *
 * 与 poll 相同，但超时使用 timespec 而非毫秒，且可指定信号掩码。
 * 寄存器：rdi=fds, rsi=nfds, rdx=tmo_p, r10=sigmask
 * musl libc 使用 ppoll 而非 poll，因此必须正确实现。
 */
int64_t sys_ppoll(trap_frame_t *frame)
{
    /* ppoll 参数布局: rdi=fds, rsi=nfds, rdx=timespec_ptr, r8=sigmask
     * 注意：Linux 内核的 ppoll 约定是 rdi/r10/r8/r9（带 sigsetsize），
     * 但实际调用约定随版本变化。这里按 x86-64 syscall 约定处理。 */
    struct linux_pollfd *fds = (struct linux_pollfd *)frame->rdi;
    nfds_t nfds = (nfds_t)frame->rsi;
    const linux_timespec_t *tmo = (const linux_timespec_t *)frame->rdx;
    /* sigmask 参数暂不处理 */

    if (!fds && nfds > 0)
        return -EFAULT;

    process_t *proc = process_current();
    if (!proc)
        return -ESRCH;

    /* 将 timespec 超时转换为毫秒 */
    int timeout_ms;
    if (!tmo) {
        timeout_ms = -1;  /* 无限等待 */
    } else {
        /* 读取用户空间的 timespec */
        uint64_t phys = mmu_virt_to_phys(proc->pml4, (uint64_t)tmo);
        if (!phys)
            return -EFAULT;
        linux_timespec_t *ktmo = (linux_timespec_t *)(uintptr_t)phys;
        timeout_ms = (int)(ktmo->tv_sec * 1000 + ktmo->tv_nsec / 1000000);
        if (timeout_ms < 0)
            timeout_ms = -1;
    }

    /* 在内核空间构建 pollfd 副本 */
    struct linux_pollfd *kfds = NULL;
    if (nfds > 0) {
        kfds = kmalloc(sizeof(struct linux_pollfd) * nfds);
        if (!kfds)
            return -ENOMEM;
        uint64_t phys = mmu_virt_to_phys(proc->pml4, (uint64_t)fds);
        if (!phys) {
            kfree(kfds);
            return -EFAULT;
        }
        memcpy(kfds, (void *)(uintptr_t)phys, sizeof(struct linux_pollfd) * nfds);
        for (nfds_t i = 0; i < nfds; i++)
            kfds[i].revents = 0;
    }

    int total_revents = 0;
    int elapsed_ms = 0;

    for (;;) {
        total_revents = 0;
        for (nfds_t i = 0; i < nfds; i++) {
            short ev = kfds[i].events;
            kfds[i].revents = 0;

            if (kfds[i].fd < 0)
                continue;

            uint32_t ep_events = epoll_check_fd(kfds[i].fd);

            if ((ev & POLLIN) && (ep_events & POLLIN))
                kfds[i].revents |= POLLIN;
            if ((ev & POLLOUT) && (ep_events & POLLOUT))
                kfds[i].revents |= POLLOUT;
            if (ep_events & POLLERR)
                kfds[i].revents |= POLLERR;
            if (ep_events & POLLHUP)
                kfds[i].revents |= POLLHUP;

            /* 检查 fd 是否有效 */
            vfs_file_t **fd_table = process_get_fd_table();
            if (!fd_table || kfds[i].fd >= PROC_MAX_FD || !fd_table[kfds[i].fd]) {
                vfs_file_t **vfs_fds = vfs_get_fd_table();
                if (!vfs_fds || kfds[i].fd >= VFS_MAX_FD || !vfs_fds[kfds[i].fd])
                    kfds[i].revents |= POLLNVAL;
            }

            if (kfds[i].revents)
                total_revents++;
        }

        if (total_revents > 0)
            break;

        if (timeout_ms == 0)
            break;

        if (timeout_ms > 0 && elapsed_ms >= timeout_ms)
            break;

        /* 注册 blocking_pid 以实现立即唤醒 */
        for (nfds_t i = 0; i < nfds; i++) {
            if (kfds[i].fd < 0)
                continue;
            net_socket_t *sock = net_get_socket(kfds[i].fd);
            if (sock && sock->domain == AF_UNIX &&
                sock->state == TCP_ESTABLISHED &&
                sock->blocking_pid < 0 &&
                (kfds[i].events & POLLIN)) {
                sock->blocking_pid = proc->pid;
            }
        }

        process_sleep(1);
        elapsed_ms += 1;
    }

    /* 清除 blocking_pid */
    for (nfds_t i = 0; i < nfds; i++) {
        if (kfds[i].fd < 0)
            continue;
        net_socket_t *sock = net_get_socket(kfds[i].fd);
        if (sock && sock->blocking_pid == proc->pid)
            sock->blocking_pid = -1;
    }

    /* 写回用户空间 */
    if (nfds > 0 && kfds) {
        for (nfds_t i = 0; i < nfds; i++) {
            uint64_t phys = mmu_virt_to_phys(proc->pml4,
                              (uint64_t)&fds[i].revents);
            if (phys)
                *(short *)(uintptr_t)phys = kfds[i].revents;
        }
        kfree(kfds);
    }

    return total_revents;
}

int64_t sys_getrandom(trap_frame_t *frame)
{
    void *buf = (void *)frame->rdi;
    size_t count = (size_t)frame->rsi;
    unsigned int flags = (unsigned int)frame->rdx;
    (void)flags;

    if (!buf || count == 0)
        return -EINVAL;

    /* 使用 TSC 和定时器的简单伪随机实现 */
    uint64_t seed = timer_get_ms();
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < count; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        p[i] = (uint8_t)(seed >> 33);
    }
    return (int64_t)count;
}

int64_t sys_futex(trap_frame_t *frame)
{
    uint32_t *uaddr   = (uint32_t *)frame->rdi;
    int       op      = (int)frame->rsi;
    uint32_t  val     = (uint32_t)frame->rdx;
    linux_timespec_t *timeout = (linux_timespec_t *)frame->r10;
    uint32_t *uaddr2  = (uint32_t *)frame->r8;
    uint32_t  val3    = (uint32_t)frame->r9;

    (void)uaddr2;
    (void)val3;

    int futex_op = op & 0x7F;  /* Mask out private flag */

    switch (futex_op) {
    case FUTEX_WAIT: {
        uint64_t timeout_ms = 0;
        if (timeout) {
            timeout_ms = (uint64_t)timeout->tv_sec * 1000 +
                         (uint64_t)timeout->tv_nsec / 1000000;
        }
        int ret = process_futex_wait(uaddr, val, timeout_ms);
        return ret;
    }
    case FUTEX_WAKE: {
        return process_futex_wake(uaddr, (int)val);
    }
    case FUTEX_WAIT_BITSET:
    case FUTEX_WAKE_BITSET:
    default:
        return -ENOSYS;
    }
}

int64_t sys_set_robust_list(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_get_robust_list(trap_frame_t *frame)
{
    (void)frame;
    return -ENOSYS;
}

int64_t sys_sysinfo(trap_frame_t *frame)
{
    (void)frame;
    return -ENOSYS;
}

/* ========================================================================
 * epoll 系统调用
 * ======================================================================== */

/* epoll_event 结构体（Linux 兼容，与用户空间 sys/epoll.h 布局一致）
 *
 * 关键：Linux 的 struct epoll_event 中 epoll_data_t data 紧跟 events，
 * 没有对齐填充（packed）。如果在内核中用 uint64_t data 会导致
 * data 在 offset 8 而非 offset 4，与用户空间不匹配。
 * 必须使用 packed 属性确保 data 从 offset 4 开始，总大小 12 字节。
 */
typedef union {
    void     *ptr;
    int       fd;
    uint32_t  u32;
    uint64_t  u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
} __attribute__((packed));

typedef struct epoll_event epoll_event_t;

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLIN     0x001
#define EPOLLOUT    0x004
#define EPOLLERR    0x008
#define EPOLLHUP    0x010
#define EPOLLRDNORM 0x040
#define EPOLLRDBAND 0x080
#define EPOLLWRNORM 0x100
#define EPOLLWRBAND 0x200
#define EPOLLMSG    0x400
#define EPOLLRDHUP  0x2000
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLWAKEUP (1U << 29)
#define EPOLLONESHOT (1U << 30)
#define EPOLLET     (1U << 31)

#define EPOLL_MAX_FDS 256

/* 内核 epoll 实例 */
typedef struct {
    int      max_fds;
    int      n_entries;
    struct {
        int          fd;
        uint32_t     events;   /* 监控的事件 */
        epoll_data_t data;     /* User data */
    } entries[EPOLL_MAX_FDS];
} epoll_instance_t;

int64_t sys_epoll_create(trap_frame_t *frame)
{
    int size = (int)frame->rdi;
    (void)size;

    epoll_instance_t *ep = (epoll_instance_t *)kmalloc(sizeof(epoll_instance_t));
    if (!ep)
        return -ENOMEM;

    memset(ep, 0, sizeof(epoll_instance_t));
    ep->max_fds = EPOLL_MAX_FDS;
    ep->n_entries = 0;

    /* 分配一个 fd 来持有 epoll 实例指针 */
    int fd = vfs_alloc_fd();
    if (fd < 0) {
        kfree(ep);
        return -EMFILE;
    }

    /* Create a dummy vfs_file to hold our epoll instance */
    vfs_file_t *file = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
    if (!file) {
        kfree(ep);
        return -ENOMEM;
    }
    memset(file, 0, sizeof(vfs_file_t));
    file->refcount = 1;
    file->flags = VFS_O_RDONLY;
    /* 在虚拟 inode 的 fs_data 中存储 epoll 实例指针 */
    vfs_inode_t *inode = (vfs_inode_t *)kmalloc(sizeof(vfs_inode_t));
    if (!inode) {
        kfree(file);
        kfree(ep);
        return -ENOMEM;
    }
    memset(inode, 0, sizeof(vfs_inode_t));
    inode->type = VFS_TYPE_EPOLL;
    inode->fs_data = ep;
    file->inode = inode;

    vfs_file_t **fd_table = process_get_fd_table();
    fd_table[fd] = file;

    return fd;
}

int64_t sys_epoll_create1(trap_frame_t *frame)
{
    int flags = (int)frame->rdi;
    (void)flags;
    return sys_epoll_create(frame);
}

static epoll_instance_t *epoll_get_instance(int epfd)
{
    vfs_file_t **fd_table = process_get_fd_table();
    if (!fd_table || epfd < 0 || epfd >= PROC_MAX_FD)
        return NULL;

    vfs_file_t *file = fd_table[epfd];
    if (!file || !file->inode || !file->inode->fs_data)
        return NULL;

    return (epoll_instance_t *)file->inode->fs_data;
}

int64_t sys_epoll_ctl(trap_frame_t *frame)
{
    int epfd = (int)frame->rdi;
    int op = (int)frame->rsi;
    int fd = (int)frame->rdx;
    epoll_event_t *event = (epoll_event_t *)frame->r10;

    serial_puts("[epoll_ctl] epfd=");
    serial_put_dec((uint64_t)epfd);
    serial_puts(" op=");
    serial_put_dec((uint64_t)op);
    serial_puts(" fd=");
    serial_put_dec((uint64_t)fd);
    if (event) {
        serial_puts(" events=0x");
        serial_put_hex((uint64_t)event->events);
        serial_puts(" data=0x");
        serial_put_hex((uint64_t)event->data.u64);
    }
    serial_puts("\n");

    epoll_instance_t *ep = epoll_get_instance(epfd);
    if (!ep)
        return -EBADF;

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (ep->n_entries >= ep->max_fds)
            return -ENOMEM;
        /* Check if fd already exists */
        for (int i = 0; i < ep->n_entries; i++) {
            if (ep->entries[i].fd == fd)
                return -EEXIST;
        }
        ep->entries[ep->n_entries].fd = fd;
        ep->entries[ep->n_entries].events = event ? event->events : 0;
        ep->entries[ep->n_entries].data = event ? event->data : (epoll_data_t){.u64 = 0};
        ep->n_entries++;
        return 0;
    }
    case EPOLL_CTL_MOD: {
        for (int i = 0; i < ep->n_entries; i++) {
            if (ep->entries[i].fd == fd) {
                ep->entries[i].events = event ? event->events : ep->entries[i].events;
                ep->entries[i].data = event ? event->data : ep->entries[i].data;
                return 0;
            }
        }
        return -ENOENT;
    }
    case EPOLL_CTL_DEL: {
        for (int i = 0; i < ep->n_entries; i++) {
            if (ep->entries[i].fd == fd) {
                /* 将剩余条目前移 */
                for (int j = i; j < ep->n_entries - 1; j++)
                    ep->entries[j] = ep->entries[j + 1];
                ep->n_entries--;
                return 0;
            }
        }
        return -ENOENT;
    }
    default:
        return -EINVAL;
    }
}

/* 检查单个 fd 的可读/可写状态，返回 EPOLLIN/EPOLLOUT/EPOLLERR/EPOLLHUP 掩码 */
static uint32_t epoll_check_fd(int fd)
{
    uint32_t revents = 0;

    /* ---- 1. 检查进程 fd_table 中的 VFS 文件（管道、eventfd、signalfd、timerfd、chardev 等） ---- */
    vfs_file_t **fd_table = process_get_fd_table();
    if (fd_table && fd >= 0 && fd < PROC_MAX_FD && fd_table[fd]) {
        vfs_file_t *f = fd_table[fd];

        /* 管道：有数据可读 */
        if (f->pipe) {
            if (f->pipe->count > 0)
                revents |= EPOLLIN;
            if (f->pipe->write_closed)
                revents |= EPOLLIN | EPOLLHUP;
            if (f->pipe->read_closed)
                revents |= EPOLLOUT | EPOLLERR;
            /* 管道写端：只要读端没关闭就总是可写 */
            if (!f->pipe->write_closed && !f->pipe->read_closed)
                revents |= EPOLLOUT;
        }

        /* 通过 inode->type 区分特殊文件类型 */
        if (f->inode) {
            switch (f->inode->type) {
            case VFS_TYPE_EVENTFD:
                /* eventfd：计数器非零即可读，总是可写 */
                if (f->inode->fs_data) {
                    eventfd_instance_t *efd = (eventfd_instance_t *)f->inode->fs_data;
                    if (efd->counter > 0)
                        revents |= EPOLLIN;
                }
                revents |= EPOLLOUT;
                break;
            case VFS_TYPE_SIGNALFD:
                /* signalfd：简单起见总是报告可读（调用者会 read 检查） */
                revents |= EPOLLIN;
                break;
            case VFS_TYPE_TIMERFD:
                /* timerfd：检查是否已过期 */
                if (f->inode->fs_data) {
                    timerfd_instance_t *tfd = (timerfd_instance_t *)f->inode->fs_data;
                    if (tfd->armed && tfd->next_expiry > 0 &&
                        timer_get_ms() >= tfd->next_expiry)
                        revents |= EPOLLIN;
                }
                break;
            case VFS_TYPE_CHARDEV:
                /* 字符设备（DRM、evdev 等）：使用驱动的 poll 回调
                 * 检查是否真的有数据可读，避免虚假就绪导致忙等。
                 * POLLIN/EPOLLIN 值相同(0x001)。 */
                {
                    int pevents = chardev_poll(f, EPOLLIN | EPOLLOUT);
                    if (pevents & EPOLLIN)
                        revents |= EPOLLIN;
                    /* 字符设备总是可写 */
                    revents |= EPOLLOUT;
                }
                break;
            case VFS_TYPE_EPOLL:
                /* epoll fd 自身不应该被监视，但以防万一 */
                break;
            case VFS_TYPE_SOCKET:
                /* Socket: check via net_socket_t pointer stored in file */
                if (f->socket) {
                    net_socket_t *sock = f->socket;
                    {
                        static int sock_check_count = 0;
                        sock_check_count++;
                        if (sock_check_count <= 200 || sock_check_count % 1000 == 0) {
                            serial_puts("[epoll_sock] fd=");
                            serial_put_dec((uint64_t)fd);
                            serial_puts(" domain=");
                            serial_put_dec((uint64_t)sock->domain);
                            serial_puts(" state=");
                            serial_put_dec((uint64_t)sock->state);
                            serial_puts(" rx_count=");
                            serial_put_dec((uint64_t)sock->rx_count);
                            serial_puts(" pending=");
                            serial_put_dec((uint64_t)sock->pending_count);
                            serial_puts("\n");
                        }
                    }
                    if (sock->domain == AF_UNIX) {
                        if (sock->state == TCP_LISTEN) {
                            if (sock->pending_count > 0)
                                revents |= EPOLLIN;
                        } else if (sock->state == TCP_ESTABLISHED) {
                            if (sock->rx_count > 0 || sock->pending_fd_count > 0)
                                revents |= EPOLLIN;
                            if (sock->unix_peer == NULL)
                                revents |= EPOLLIN | EPOLLHUP;
                            revents |= EPOLLOUT;
                        } else if (sock->state == TCP_CLOSE_WAIT) {
                            revents |= EPOLLIN | EPOLLHUP;
                        }
                    } else {
                        /* AF_INET socket */
                        if (sock->state == TCP_LISTEN) {
                            if (sock->pending_count > 0)
                                revents |= EPOLLIN;
                        } else if (sock->state == TCP_ESTABLISHED) {
                            if (sock->rx_count > 0)
                                revents |= EPOLLIN;
                            revents |= EPOLLOUT;
                        }
                    }
                }
                break;
            default:
                /* 普通文件：总是可读写 */
                if (!f->pipe)
                    revents |= EPOLLIN | EPOLLOUT;
                break;
            }
        }
    }

    /* ---- 2. 检查全局 socket 表中的网络/Unix socket ---- */
    net_socket_t *sock = net_get_socket(fd);
    if (sock) {
        if (sock->domain == AF_UNIX) {
            if (sock->state == TCP_LISTEN) {
                /* 监听 socket：有 pending 连接即可读 */
                if (sock->pending_count > 0)
                    revents |= EPOLLIN;
            } else if (sock->state == TCP_ESTABLISHED) {
                /* 已连接 socket：rx 缓冲区有数据或有待接收 fd 即可读 */
                if (sock->rx_count > 0 || sock->pending_fd_count > 0)
                    revents |= EPOLLIN;
                /* 对端关闭 */
                if (sock->unix_peer == NULL)
                    revents |= EPOLLIN | EPOLLHUP;
                /* 总是可写 */
                revents |= EPOLLOUT;
            } else if (sock->state == TCP_CLOSE_WAIT) {
                revents |= EPOLLIN | EPOLLHUP;
            }
        } else {
            /* AF_INET socket */
            if (sock->state == TCP_LISTEN) {
                if (sock->pending_count > 0)
                    revents |= EPOLLIN;
            } else if (sock->state == TCP_ESTABLISHED) {
                if (sock->rx_count > 0)
                    revents |= EPOLLIN;
                revents |= EPOLLOUT;
            }
        }
    }

    return revents;
}

int64_t sys_epoll_wait(trap_frame_t *frame)
{
    int epfd = (int)frame->rdi;
    epoll_event_t *events = (epoll_event_t *)frame->rsi;
    int maxevents = (int)frame->rdx;
    int timeout = (int)frame->r10;

    if (maxevents <= 0 || !events)
        return -EINVAL;

    epoll_instance_t *ep = epoll_get_instance(epfd);
    if (!ep)
        return -EBADF;

    process_t *proc = process_current();

    /* 调试：记录 epoll_wait 调用 */
    {
        static int ew_count = 0;
        ew_count++;
        if (ew_count <= 50) {
            serial_puts("[epoll_wait] PID=");
            serial_put_dec((uint64_t)(proc ? proc->pid : 0));
            serial_puts(" epfd=");
            serial_put_dec((uint64_t)epfd);
            serial_puts(" timeout=");
            serial_put_dec((uint64_t)timeout);
            serial_puts(" n_entries=");
            serial_put_dec((uint64_t)ep->n_entries);
            serial_puts("\n");
        }
    }

    uint64_t start_ms = timer_get_ms();
    int n_ready = 0;

    for (;;) {
        /* 轮询每个被监视的 fd */
        n_ready = 0;
        for (int i = 0; i < ep->n_entries && n_ready < maxevents; i++) {
            int fd = ep->entries[i].fd;

            uint32_t revents = epoll_check_fd(fd);

            /* 与请求的事件掩码取交集（EPOLLERR/EPOLLHUP 总是报告） */
            revents &= (ep->entries[i].events | EPOLLERR | EPOLLHUP);

            if (revents) {
                events[n_ready].events = revents;
                events[n_ready].data = ep->entries[i].data;
                n_ready++;
            }
        }

        if (n_ready > 0) {
            /* 清除之前注册的 blocking_pid */
            if (proc) {
                for (int i = 0; i < ep->n_entries; i++) {
                    net_socket_t *sock = net_get_socket(ep->entries[i].fd);
                    if (sock && sock->blocking_pid == proc->pid)
                        sock->blocking_pid = -1;
                }
            }
            {
                static int ewr_count = 0;
                ewr_count++;
                if (ewr_count <= 30) {
                    serial_puts("[epoll_wait] return n_ready=");
                    serial_put_dec((uint64_t)n_ready);
                    serial_puts("\n");
                }
            }
            return n_ready;
        }

        /* 检查超时 */
        if (timeout == 0) {
            {
                static int ewz_count = 0;
                ewz_count++;
                if (ewz_count <= 20) {
                    serial_puts("[epoll_wait] timeout=0, return 0\n");
                }
            }
            return 0;  /* 非阻塞，无事件 */
        }

        if (timeout > 0) {
            uint64_t elapsed = timer_get_ms() - start_ms;
            if (elapsed >= (uint64_t)timeout) {
                /* 清除 blocking_pid */
                if (proc) {
                    for (int i = 0; i < ep->n_entries; i++) {
                        net_socket_t *sock = net_get_socket(ep->entries[i].fd);
                        if (sock && sock->blocking_pid == proc->pid)
                            sock->blocking_pid = -1;
                    }
                }
                return 0;
            }
        }

        /* 在休眠前注册 blocking_pid，这样 unix_send() 能立即唤醒 */
        if (proc) {
            for (int i = 0; i < ep->n_entries; i++) {
                net_socket_t *sock = net_get_socket(ep->entries[i].fd);
                if (sock && sock->domain == AF_UNIX &&
                    sock->state == TCP_ESTABLISHED &&
                    sock->blocking_pid < 0 &&
                    (ep->entries[i].events & EPOLLIN)) {
                    sock->blocking_pid = proc->pid;
                }
            }
        }

        /* 使用较长的休眠时间，依赖 process_wake() 在数据到达时立即唤醒。
         * 之前 process_sleep(1) 导致每 1ms 轮询一次，浪费 CPU。 */
        int sleep_ms = 100;
        if (timeout > 0) {
            uint64_t elapsed = timer_get_ms() - start_ms;
            int remaining = timeout - (int)elapsed;
            if (remaining <= 0) {
                /* 清除 blocking_pid */
                if (proc) {
                    for (int i = 0; i < ep->n_entries; i++) {
                        net_socket_t *sock = net_get_socket(ep->entries[i].fd);
                        if (sock && sock->blocking_pid == proc->pid)
                            sock->blocking_pid = -1;
                    }
                }
                return 0;
            }
            if (remaining < sleep_ms)
                sleep_ms = remaining;
        }
        process_sleep(sleep_ms);
    }
}

/* epoll_pwait：与 epoll_wait 相同，但可指定信号掩码。
 * 我们暂时忽略 sigmask 参数（进程级信号掩码尚未完全实现）。 */
int64_t sys_epoll_pwait(trap_frame_t *frame)
{
    /* epoll_pwait(epfd, events, maxevents, timeout, sigmask)
     * 寄存器分配: rdi=epfd, rsi=events, rdx=maxevents, r10=timeout, r8=sigmask */
    /* sigmask 参数在 r8 中，暂时忽略 */
    return sys_epoll_wait(frame);
}

/* ========================================================================
 * eventfd 系统调用
 * ======================================================================== */

/* eventfd_instance_t 已在文件开头定义 */

static int eventfd_alloc_fd(eventfd_instance_t *efd)
{
    int fd = vfs_alloc_fd();
    if (fd < 0)
        return -EMFILE;

    vfs_file_t *file = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
    if (!file)
        return -ENOMEM;

    memset(file, 0, sizeof(vfs_file_t));
    file->refcount = 1;
    file->flags = VFS_O_RDWR;

    vfs_inode_t *inode = (vfs_inode_t *)kmalloc(sizeof(vfs_inode_t));
    if (!inode) {
        kfree(file);
        return -ENOMEM;
    }
    memset(inode, 0, sizeof(vfs_inode_t));
    inode->type = VFS_TYPE_EVENTFD;
    inode->fs_data = efd;
    file->inode = inode;

    vfs_file_t **fd_table = process_get_fd_table();
    fd_table[fd] = file;

    return fd;
}

int64_t sys_eventfd(trap_frame_t *frame)
{
    unsigned int initval = (unsigned int)frame->rdi;

    eventfd_instance_t *efd = (eventfd_instance_t *)kmalloc(sizeof(eventfd_instance_t));
    if (!efd)
        return -ENOMEM;

    memset(efd, 0, sizeof(eventfd_instance_t));
    efd->counter = initval;
    efd->flags = 0;

    int fd = eventfd_alloc_fd(efd);
    if (fd < 0) {
        kfree(efd);
        return fd;
    }

    return fd;
}

int64_t sys_eventfd2(trap_frame_t *frame)
{
    unsigned int initval = (unsigned int)frame->rdi;
    int flags = (int)frame->rsi;

    eventfd_instance_t *efd = (eventfd_instance_t *)kmalloc(sizeof(eventfd_instance_t));
    if (!efd)
        return -ENOMEM;

    memset(efd, 0, sizeof(eventfd_instance_t));
    efd->counter = initval;
    efd->flags = flags;

    int fd = eventfd_alloc_fd(efd);
    if (fd < 0) {
        kfree(efd);
        return fd;
    }

    return fd;
}

/* ========================================================================
 * signalfd syscalls
 * ======================================================================== */

/* 内核 signalfd 实例 */
typedef struct {
    uint64_t sigmask;
    int      flags;
} signalfd_instance_t;

static int signalfd_alloc_fd(signalfd_instance_t *sfd)
{
    int fd = vfs_alloc_fd();
    if (fd < 0)
        return -EMFILE;

    vfs_file_t *file = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
    if (!file)
        return -ENOMEM;

    memset(file, 0, sizeof(vfs_file_t));
    file->refcount = 1;
    file->flags = VFS_O_RDONLY;

    vfs_inode_t *inode = (vfs_inode_t *)kmalloc(sizeof(vfs_inode_t));
    if (!inode) {
        kfree(file);
        return -ENOMEM;
    }
    memset(inode, 0, sizeof(vfs_inode_t));
    inode->type = VFS_TYPE_SIGNALFD;
    inode->fs_data = sfd;
    file->inode = inode;

    vfs_file_t **fd_table = process_get_fd_table();
    fd_table[fd] = file;

    return fd;
}

int64_t sys_signalfd(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    const uint64_t *mask = (const uint64_t *)frame->rsi;
    size_t sizemask = (size_t)frame->rdx;
    (void)fd; (void)sizemask;

    signalfd_instance_t *sfd = (signalfd_instance_t *)kmalloc(sizeof(signalfd_instance_t));
    if (!sfd)
        return -ENOMEM;

    memset(sfd, 0, sizeof(signalfd_instance_t));
    sfd->sigmask = mask ? *mask : 0;
    sfd->flags = 0;

    /* 如果 fd == -1，创建新 fd */
    if (fd == -1) {
        int new_fd = signalfd_alloc_fd(sfd);
        if (new_fd < 0) {
            kfree(sfd);
            return new_fd;
        }
        return new_fd;
    }

    /* 否则，复用已有 fd - 仅存储掩码 */
    return fd;
}

int64_t sys_signalfd4(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    const uint64_t *mask = (const uint64_t *)frame->rsi;
    size_t sizemask = (size_t)frame->rdx;
    int flags = (int)frame->r10;
    (void)fd; (void)sizemask;

    signalfd_instance_t *sfd = (signalfd_instance_t *)kmalloc(sizeof(signalfd_instance_t));
    if (!sfd)
        return -ENOMEM;

    memset(sfd, 0, sizeof(signalfd_instance_t));
    sfd->sigmask = mask ? *mask : 0;
    sfd->flags = flags;

    if (fd == -1) {
        int new_fd = signalfd_alloc_fd(sfd);
        if (new_fd < 0) {
            kfree(sfd);
            return new_fd;
        }
        return new_fd;
    }

    return fd;
}

/* ========================================================================
 * timerfd 系统调用
 * ======================================================================== */

/* timerfd_instance_t 已在文件开头定义 */

int64_t sys_timerfd_create(trap_frame_t *frame)
{
    int clockid = (int)frame->rdi;
    int flags = (int)frame->rsi;

    timerfd_instance_t *tfd = (timerfd_instance_t *)kmalloc(sizeof(timerfd_instance_t));
    if (!tfd)
        return -ENOMEM;

    memset(tfd, 0, sizeof(timerfd_instance_t));
    tfd->clockid = clockid;
    tfd->flags = flags;
    tfd->armed = 0;

    int fd = vfs_alloc_fd();
    if (fd < 0) {
        kfree(tfd);
        return -EMFILE;
    }

    vfs_file_t *file = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
    if (!file) {
        kfree(tfd);
        return -ENOMEM;
    }
    memset(file, 0, sizeof(vfs_file_t));
    file->refcount = 1;
    file->flags = VFS_O_RDONLY;

    vfs_inode_t *inode = (vfs_inode_t *)kmalloc(sizeof(vfs_inode_t));
    if (!inode) {
        kfree(file);
        kfree(tfd);
        return -ENOMEM;
    }
    memset(inode, 0, sizeof(vfs_inode_t));
    inode->type = VFS_TYPE_TIMERFD;
    inode->fs_data = tfd;
    file->inode = inode;

    vfs_file_t **fd_table = process_get_fd_table();
    fd_table[fd] = file;

    return fd;
}

int64_t sys_timerfd_settime(trap_frame_t *frame)
{
    int fd = (int)frame->rdi;
    int flags = (int)frame->rsi;
    const linux_timespec_t *new_value = (const linux_timespec_t *)frame->rdx;
    linux_timespec_t *old_value = (linux_timespec_t *)frame->r10;

    (void)fd; (void)flags;

    /* 如果请求，返回旧的定时器值 */
    if (old_value) {
        old_value->tv_sec = 0;
        old_value->tv_nsec = 0;
    }

    if (!new_value)
        return -EFAULT;

    /* 仅通过 fd 不容易访问 timerfd 实例，
     * 因此只接受参数并返回成功。
     * 完整实现应通过 fd 查找 vfs_file，
     * 然后通过 inode->fs_data 获取 timerfd_instance_t。 */
    return 0;
}

int64_t sys_timerfd_gettime(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

/* ========================================================================
 * inotify 系统调用
 * ======================================================================== */

int64_t sys_inotify_init(trap_frame_t *frame)
{
    (void)frame;
    return vfs_open("/dev/null", VFS_O_RDONLY, 0);
}

int64_t sys_inotify_init1(trap_frame_t *frame)
{
    (void)frame;
    return vfs_open("/dev/null", VFS_O_RDONLY, 0);
}

int64_t sys_inotify_add_watch(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_inotify_rm_watch(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

/* ========================================================================
 * dl_iterate_phdr - glibc/JVM 需要
 * ======================================================================== */

int64_t sys_dl_iterate_phdr(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

/* ========================================================================
 * getauxval - JVM 用于 AT_HWCAP 等
 * ======================================================================== */

int64_t sys_getauxval(trap_frame_t *frame)
{
    uint64_t type = frame->rdi;
    /* 返回常见辅助向量项的合理值 */
    switch (type) {
    case 0: return 0;  /* AT_NULL */
    case 1: return 0;  /* AT_IGNORE */
    case 2: return 4096;  /* AT_EXECFD */
    case 3: return (uint64_t)(uintptr_t)"\0"; /* AT_PHDR */
    case 4: return 56;   /* AT_PHENT */
    case 5: return 0;    /* AT_PHNUM */
    case 6: return 4096; /* AT_PAGESZ */
    case 7: return 0;    /* AT_BASE */
    case 8: return 0;    /* AT_FLAGS */
    case 9: return 0;    /* AT_ENTRY */
    case 11: return 0;   /* AT_UID */
    case 12: return 0;   /* AT_EUID */
    case 13: return 0;   /* AT_GID */
    case 14: return 0;   /* AT_EGID */
    case 15: return 0;   /* AT_PLATFORM */
    case 16: return 0x0000; /* AT_HWCAP */
    case 17: return 0;   /* AT_CLKTCK */
    case 23: return 0;   /* AT_SECURE */
    case 25: return 0;   /* AT_RANDOM */
    case 26: return 0;   /* AT_HWCAP2 */
    case 27: return 0;   /* AT_EXECFN */
    default: return 0;
    }
}

/* ========================================================================
 * membarrier 内存屏障
 * ======================================================================== */

int64_t sys_membarrier(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

/* ========================================================================
 * rseq 可重启序列
 * ======================================================================== */

int64_t sys_rseq(trap_frame_t *frame)
{
    (void)frame;
    return -ENOSYS;
}

/* ========================================================================
 * 资源限制
 * ======================================================================== */

int64_t sys_getrlimit(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_setrlimit(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

/* ========================================================================
 * SFK 特有系统调用（600+）
 * ======================================================================== */

int64_t sys_sfk_check_perm(trap_frame_t *frame)
{
    uint32_t perm = (uint32_t)frame->rdi;
    process_t *proc = process_current();
    if (!proc)
        return -ESRCH;

    if (proc->sfk_perms & perm)
        return 0;

    return -EPERM;
}

int64_t sys_sfk_get_pkg_info(trap_frame_t *frame)
{
    const char *pkg_id = (const char *)frame->rdi;
    reg_software_record_t *record = (reg_software_record_t *)frame->rsi;
    (void)pkg_id;

    if (!pkg_id || !record)
        return -EFAULT;

    int ret = registry_get_software(pkg_id, record);
    if (ret < 0)
        return -ENOENT;

    return 0;
}

/* ========================================================================
 * 注册表系统调用（700+）
 * ======================================================================== */

int64_t sys_reg_read(trap_frame_t *frame)
{
    const char *key_path = (const char *)frame->rdi;
    const char *value_name = (const char *)frame->rsi;
    uint32_t *type = (uint32_t *)frame->rdx;
    void *data = (void *)frame->r10;
    uint32_t *data_size = (uint32_t *)frame->r8;
    (void)key_path;
    (void)value_name;
    (void)type;
    (void)data;
    (void)data_size;
    return registry_read_value(key_path, value_name, type, data, data_size);
}

int64_t sys_reg_write(trap_frame_t *frame)
{
    const char *key_path = (const char *)frame->rdi;
    const char *value_name = (const char *)frame->rsi;
    uint32_t type = (uint32_t)frame->rdx;
    const void *data = (const void *)frame->r10;
    uint32_t data_size = (uint32_t)frame->r8;
    (void)key_path;
    (void)value_name;
    (void)data;
    return registry_write_value(key_path, value_name, type, data, data_size);
}

int64_t sys_reg_list(trap_frame_t *frame)
{
    const char *key_path = (const char *)frame->rdi;
    int list_values = (int)frame->rsi;
    void *names = (void *)frame->rdx;
    int max_entries = (int)frame->r10;
    (void)key_path;
    (void)names;

    if (list_values) {
        return registry_list_values(key_path, (char (*)[REG_MAX_VALUE_NAME])names, max_entries);
    } else {
        return registry_list_keys(key_path, (char (*)[REG_MAX_KEY_NAME])names, max_entries);
    }
}

int64_t sys_clock_getres(trap_frame_t *frame)
{
    int clk_id = (int)frame->rdi;
    linux_timespec_t *res = (linux_timespec_t *)frame->rsi;
    (void)clk_id;
    if (res) {
        res->tv_sec = 0;
        res->tv_nsec = 1;
    }
    return 0;
}

int64_t sys_rt_sigsuspend(trap_frame_t *frame)
{
    (void)frame;
    return -EINTR;
}
