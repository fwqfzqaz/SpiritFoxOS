/*
 * evdev.c - SpiritFoxOS evdev 字符设备接口
 *
 * 为每个已注册的输入设备在 /dev/input/eventN 创建字符设备节点，
 * 提供与 Linux evdev 兼容的 read/ioctl/poll 语义。
 *
 * 每个设备最多支持 EVDEV_MAX_CLIENTS 个客户端（多读），每个客户端
 * 拥有独立的事件环形缓冲区。input_sync() 通过 evdev_dispatch()
 * 把整帧事件扇出复制到所有客户端缓冲区。
 *
 * 客户端私有数据通过 vfs_file_t::dir_index 字段保存其在设备
 * 客户端表中的槽位号（该字段仅用于目录读取，对字符设备无影响）。
 */

#include "input.h"
#include "chardev.h"
#include "memory.h"
#include "string.h"
#include "vga.h"
#include "timer.h"
#include "process.h"
#include "errno.h"

/* ========================================================================
 * _IOC 宏兼容层
 *
 * input.h 中的 EVIOC* 宏依赖 _IOR/_IOW/_IOC/_IOC_READ，这些宏在
 * 本内核中定义于 drm.h。为避免在输入驱动中引入 DRM 头文件，此处
 * 提供与 Linux asm-generic/ioctl.h 等价的兼容定义。
 * 标准 Linux ioctl 编码 (x86_64):
 *   bits 7:0   = nr   (命令编号)
 *   bits 15:8  = type (类型魔数)
 *   bits 29:16 = size (参数大小, 14 bits)
 *   bits 31:30 = dir  (方向, 2 bits)
 * ======================================================================== */
#ifndef _IOC_NRBITS
#define _IOC_NRBITS     8
#define _IOC_TYPEBITS   8
#define _IOC_SIZEBITS   14
#define _IOC_DIRBITS    2

#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  _IOC_NRBITS
#define _IOC_SIZESHIFT  (_IOC_NRBITS + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT   (_IOC_NRBITS + _IOC_TYPEBITS + _IOC_SIZEBITS)

#define _IOC_NRMASK     ((1U << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK   ((1U << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK   ((1U << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK    ((1U << _IOC_DIRBITS) - 1)

#define _IOC_NONE       0
#define _IOC_WRITE      1
#define _IOC_READ       2

#define _IOC(dir, type, nr, size) \
    (((dir)  << _IOC_DIRSHIFT) | \
     ((type) << _IOC_TYPESHIFT) | \
     ((nr)   << _IOC_NRSHIFT) | \
     ((size) & _IOC_SIZEMASK))

#define _IO(type, nr)             _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOW(type, nr, size)      _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#define _IOR(type, nr, size)      _IOC(_IOC_READ,  (type), (nr), sizeof(size))
#define _IOWR(type, nr, size)     _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(size))

#define _IOC_NR(cmd)   (((cmd) >> _IOC_NRSHIFT)  & _IOC_NRMASK)
#define _IOC_TYPE(cmd) (((cmd) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_SIZE(cmd) (((cmd) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)
#define _IOC_DIR(cmd)  (((cmd) >> _IOC_DIRSHIFT)  & _IOC_DIRMASK)
#endif /* _IOC_NRBITS */

/* ========================================================================
 * 内部常量
 * ======================================================================== */

#define EVDEV_MAX_DEVICES   32      /* 最多输入设备数（对应次设备号 0..31） */
#define EVDEV_MAX_CLIENTS   8       /* 每个设备最多客户端数（多读） */
#define EVDEV_BUF_SIZE      128     /* 客户端环形缓冲区事件数（单页可容纳） */

/* Linux open() 非阻塞标志（vfs.h 未定义，此处补充） */
#ifndef O_NONBLOCK
#define O_NONBLOCK         00004000
#endif

/* poll 事件位 */
#ifndef POLLIN
#define POLLIN             0x0001
#endif

/* ioctl 命令编号（_IOC_NR 部分） */
#define EVDEV_IOCTL_VERSION  0x01
#define EVDEV_IOCTL_ID       0x02
#define EVDEV_IOCTL_NAME     0x06
#define EVDEV_IOCTL_PHYS     0x07
#define EVDEV_IOCTL_SETLED   0x0b
#define EVDEV_IOCTL_GETKEY   0x18
#define EVDEV_IOCTL_GETLED   0x19
#define EVDEV_IOCTL_BIT_BASE 0x20   /* EVIOCGBIT(ev,len): nr = 0x20 + ev */
#define EVDEV_IOCTL_ABS_BASE 0x40   /* EVIOCGABS(abs):    nr = 0x40 + abs */

/* ========================================================================
 * 数据结构
 * ======================================================================== */

/* evdev 客户端：每个打开的文件描述符拥有一个 */
typedef struct {
    input_event_t buf[EVDEV_BUF_SIZE];   /* 环形缓冲区 */
    int read_idx;                        /* 下一个可读位置 */
    int write_idx;                       /* 下一个写入位置 */
    int count;                           /* 当前缓冲事件数 */
    int dropped;                         /* 事件丢弃标志 */
} evdev_client_t;

/* 每个输入设备的 evdev 状态 */
typedef struct {
    input_dev_t       *dev;                              /* 关联的输入设备 */
    evdev_client_t    *clients[EVDEV_MAX_CLIENTS];       /* 客户端表 */
    chardev_ops_t      ops;                              /* 字符设备操作表 */
    int                in_use;                           /* 该槽位是否启用 */
} evdev_state_t;

/* ========================================================================
 * 全局状态
 * ======================================================================== */

static evdev_state_t evdev_states[EVDEV_MAX_DEVICES];

/* ========================================================================
 * 内部辅助：从文件取得设备状态
 * ======================================================================== */

static evdev_state_t *evdev_state_from_file(vfs_file_t *file)
{
    if (!file || !file->inode)
        return NULL;

    uint32_t id = file->inode->chardev_id;
    if (id < CHARDEV_ID_INPUT_BASE || id >= CHARDEV_ID_INPUT_MAX)
        return NULL;

    int minor = (int)(id - CHARDEV_ID_INPUT_BASE);
    if (minor < 0 || minor >= EVDEV_MAX_DEVICES)
        return NULL;

    if (!evdev_states[minor].in_use)
        return NULL;

    return &evdev_states[minor];
}

/* ========================================================================
 * evdev 客户端操作
 * ======================================================================== */

static int evdev_open(vfs_file_t *file)
{
    evdev_state_t *st = evdev_state_from_file(file);
    if (!st)
        return -1;

    /* 查找空闲客户端槽位 */
    int slot = -1;
    for (int i = 0; i < EVDEV_MAX_CLIENTS; i++) {
        if (st->clients[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;  /* 客户端过多 */

    evdev_client_t *c = (evdev_client_t *)alloc_page();
    if (!c)
        return -1;
    memset(c, 0, sizeof(*c));

    st->clients[slot] = c;

    /* 利用 dir_index（仅目录读取使用）保存客户端槽位号 */
    file->dir_index = (uint32_t)slot;

    return 0;
}

static int evdev_close(vfs_file_t *file)
{
    evdev_state_t *st = evdev_state_from_file(file);
    if (!st)
        return -1;

    int slot = (int)file->dir_index;
    if (slot < 0 || slot >= EVDEV_MAX_CLIENTS)
        return -1;

    if (st->clients[slot]) {
        free_page(st->clients[slot]);
        st->clients[slot] = NULL;
    }
    file->dir_index = 0;

    return 0;
}

static int evdev_read(vfs_file_t *file, void *buf, size_t count)
{
    evdev_state_t *st = evdev_state_from_file(file);
    if (!st || !buf)
        return -1;

    int slot = (int)file->dir_index;
    if (slot < 0 || slot >= EVDEV_MAX_CLIENTS || !st->clients[slot])
        return -1;

    evdev_client_t *c = st->clients[slot];

    /* 必须至少能容纳一个完整事件 */
    if (count < sizeof(input_event_t))
        return 0;

    int max_events = (int)(count / sizeof(input_event_t));
    int nonblock = (file->flags & O_NONBLOCK) ? 1 : 0;

    /* 无事件且非非阻塞时阻塞等待 */
    while (c->count == 0) {
        if (nonblock)
            return -EAGAIN;
        process_sleep(1);
    }

    /* 从环形缓冲区读取事件 */
    input_event_t *out = (input_event_t *)buf;
    int events_read = 0;
    while (events_read < max_events && c->count > 0) {
        out[events_read] = c->buf[c->read_idx];
        c->read_idx = (c->read_idx + 1) % EVDEV_BUF_SIZE;
        c->count--;
        events_read++;
    }

    return events_read * (int)sizeof(input_event_t);
}

static int64_t evdev_ioctl(vfs_file_t *file, uint64_t cmd, uint64_t arg)
{
    evdev_state_t *st = evdev_state_from_file(file);
    if (!st || !st->dev)
        return -ENOTTY;

    input_dev_t *dev = st->dev;

    /* 截断为 32 位以处理用户空间符号扩展 */
    cmd &= 0xFFFFFFFFU;

    /* 仅处理 'E' 类型 ioctl */
    if (_IOC_TYPE(cmd) != 'E')
        return -ENOTTY;

    uint8_t nr = (uint8_t)_IOC_NR(cmd);
    uint16_t size = (uint16_t)_IOC_SIZE(cmd);
    void *uptr = (void *)(uintptr_t)arg;

    switch (nr) {
    case EVDEV_IOCTL_VERSION:  /* EVIOCGVERSION */
        if (arg)
            *(int *)uptr = EV_VERSION;
        return 0;

    case EVDEV_IOCTL_ID:  /* EVIOCGID */
        if (arg)
            memcpy(uptr, &dev->id, sizeof(input_id_t));
        return 0;

    case EVDEV_IOCTL_NAME: {  /* EVIOCGNAME(len) */
        int n = size;
        if (n > (int)sizeof(dev->name))
            n = (int)sizeof(dev->name);
        if (arg && n > 0)
            memcpy(uptr, dev->name, n);
        return n;
    }

    case EVDEV_IOCTL_PHYS: {  /* EVIOCGPHYS(len) */
        int n = size;
        if (n > (int)sizeof(dev->phys))
            n = (int)sizeof(dev->phys);
        if (arg && n > 0)
            memcpy(uptr, dev->phys, n);
        return n;
    }

    case EVDEV_IOCTL_GETKEY: {  /* EVIOCGKEY(len) */
        int n = size;
        int bl = (int)sizeof(dev->key_state);
        if (n > bl)
            n = bl;
        if (arg && n > 0)
            memcpy(uptr, dev->key_state, n);
        return n;
    }

    case EVDEV_IOCTL_GETLED: {  /* EVIOCGLED(len) */
        int n = size;
        int bl = (int)sizeof(dev->led_state);
        if (n > bl)
            n = bl;
        if (arg && n > 0)
            memcpy(uptr, dev->led_state, n);
        return n;
    }

    case EVDEV_IOCTL_SETLED: {  /* EVIOCSLED：arg -> int[2] = {led, value} */
        if (!arg)
            return -EINVAL;
        int *p = (int *)uptr;
        uint16_t led = (uint16_t)p[0];
        int32_t value = p[1];
        return input_set_led(dev, led, value);
    }

    default:
        break;
    }

    /* EVIOCGBIT(ev, len): nr = 0x20 + ev */
    if (nr >= EVDEV_IOCTL_BIT_BASE && nr < EVDEV_IOCTL_ABS_BASE) {
        int ev = nr - EVDEV_IOCTL_BIT_BASE;
        const void *src = NULL;
        int bl = 0;

        switch (ev) {
        case 0:        /* EV_SYN -> 返回 evbit */
            src = dev->evbit;
            bl = (int)sizeof(dev->evbit);
            break;
        case EV_KEY:
            src = dev->keybit;
            bl = (int)sizeof(dev->keybit);
            break;
        case EV_REL:
            src = dev->relbit;
            bl = (int)sizeof(dev->relbit);
            break;
        case EV_ABS:
            src = dev->absbit;
            bl = (int)sizeof(dev->absbit);
            break;
        case EV_LED:
            src = dev->ledbit;
            bl = (int)sizeof(dev->ledbit);
            break;
        default:
            break;
        }

        int n = size;
        if (n > bl)
            n = bl;
        if (arg && n > 0 && src)
            memcpy(uptr, src, n);
        return n;
    }

    /* EVIOCGABS(abs): nr = 0x40 + abs */
    if (nr >= EVDEV_IOCTL_ABS_BASE && nr < 0x80) {
        int abs = nr - EVDEV_IOCTL_ABS_BASE;
        if (abs < 0 || abs >= ABS_CNT)
            return -EINVAL;
        if (arg)
            memcpy(uptr, &dev->absinfo[abs], sizeof(input_absinfo_t));
        return 0;
    }

    return -ENOTTY;
}

static int evdev_poll(vfs_file_t *file, int events)
{
    evdev_state_t *st = evdev_state_from_file(file);
    if (!st)
        return 0;

    int slot = (int)file->dir_index;
    if (slot < 0 || slot >= EVDEV_MAX_CLIENTS || !st->clients[slot])
        return 0;

    evdev_client_t *c = st->clients[slot];

    if ((events & POLLIN) && c->count > 0)
        return POLLIN;

    return 0;
}

/* ========================================================================
 * evdev 子系统接口
 * ======================================================================== */

void evdev_init(void)
{
    memset(evdev_states, 0, sizeof(evdev_states));
    printf("[evdev] evdev subsystem initialized\n");
}

void evdev_register_device(input_dev_t *dev)
{
    if (!dev)
        return;
    if (dev->evdev_minor < 0 || dev->evdev_minor >= EVDEV_MAX_DEVICES)
        return;

    evdev_state_t *st = &evdev_states[dev->evdev_minor];
    memset(st, 0, sizeof(*st));
    st->dev = dev;
    st->in_use = 1;

    /* 填充字符设备操作表 */
    st->ops.open  = evdev_open;
    st->ops.close = evdev_close;
    st->ops.read  = evdev_read;
    st->ops.write = NULL;
    st->ops.ioctl = evdev_ioctl;
    st->ops.mmap  = NULL;
    st->ops.poll  = evdev_poll;

    /* 注册字符设备 */
    dev->chardev_id = CHARDEV_ID_INPUT_BASE + (uint32_t)dev->evdev_minor;
    chardev_register(dev->chardev_id, &st->ops);

    /* 创建 /dev/input/eventN 设备节点 */
    char path[32];
    strcpy(path, "/dev/input/event");
    itoa(dev->evdev_minor, path + strlen(path), 10);
    chardev_create_node(path, dev->chardev_id,
                        VFS_S_IRUSR | VFS_S_IWUSR);

    printf("[evdev] registered %s -> %s (chardev %u)\n",
           dev->name[0] ? dev->name : "?", path, dev->chardev_id);
}

void evdev_dispatch(input_dev_t *dev, input_event_t *events, int count)
{
    if (!dev || !events || count <= 0)
        return;
    if (dev->evdev_minor < 0 || dev->evdev_minor >= EVDEV_MAX_DEVICES)
        return;

    evdev_state_t *st = &evdev_states[dev->evdev_minor];
    if (!st->in_use)
        return;

    /* 把整帧事件扇出复制到每个客户端的环形缓冲区 */
    for (int i = 0; i < EVDEV_MAX_CLIENTS; i++) {
        evdev_client_t *c = st->clients[i];
        if (!c)
            continue;

        for (int e = 0; e < count; e++) {
            if (c->count >= EVDEV_BUF_SIZE) {
                /* 缓冲区满：丢弃最旧事件 */
                c->read_idx = (c->read_idx + 1) % EVDEV_BUF_SIZE;
                c->count--;
                c->dropped = 1;
            }
            c->buf[c->write_idx] = events[e];
            c->write_idx = (c->write_idx + 1) % EVDEV_BUF_SIZE;
            c->count++;
        }
    }
}
