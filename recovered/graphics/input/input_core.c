/*
 * input_core.c - SpiritFoxOS 输入核心层
 *
 * 提供 Linux evdev 兼容的输入设备注册、能力管理与事件上报框架。
 * 设备驱动（键盘、鼠标等）通过 input_allocate_device() 分配设备，
 * 用 input_set_capability() 声明能力，再 input_register_device() 注册，
 * 随后即可通过 input_report_key/rel/abs() + input_sync() 上报事件。
 *
 * input_sync() 会把当前帧缓冲区中的所有事件通过 evdev_dispatch()
 * 分发给打开对应 /dev/input/eventN 的所有 evdev 客户端。
 */

#include "input.h"
#include "chardev.h"
#include "memory.h"
#include "string.h"
#include "vga.h"
#include "timer.h"

/* ========================================================================
 * 内部常量
 * ======================================================================== */

#define INPUT_MAX_DEVICES   32      /* 最多支持的输入设备数 */
#define INPUT_FRAME_SIZE    64      /* 每个设备当前帧最多缓存的事件数 */

/* 每个设备的“当前帧”临时缓冲区，按 evdev_minor 索引 */
typedef struct {
    input_event_t events[INPUT_FRAME_SIZE];
    int count;
} input_frame_t;

/* ========================================================================
 * 全局状态
 * ======================================================================== */

/* 已注册的输入设备链表头 */
static input_dev_t *input_dev_list = NULL;

/* evdev 次设备号分配计数器（0, 1, 2 ...） */
static int evdev_minor_counter = 0;

/* 每个设备的当前帧缓冲区，按 evdev_minor 索引 */
static input_frame_t input_frames[INPUT_MAX_DEVICES];

/* ========================================================================
 * 初始化
 * ======================================================================== */

void input_init(void)
{
    input_dev_list = NULL;
    evdev_minor_counter = 0;
    memset(input_frames, 0, sizeof(input_frames));

    /* 初始化 evdev 字符设备接口 */
    evdev_init();

    printf("[INPUT] input subsystem initialized\n");
}

/* ========================================================================
 * 设备分配与注册
 * ======================================================================== */

input_dev_t *input_allocate_device(void)
{
    input_dev_t *dev = (input_dev_t *)alloc_page();
    if (!dev)
        return NULL;

    memset(dev, 0, PAGE_SIZE);
    dev->evdev_minor = -1;
    dev->chardev_id = 0;
    dev->private = NULL;
    dev->next = NULL;

    return dev;
}

int input_register_device(input_dev_t *dev)
{
    if (!dev)
        return -1;

    /* 分配 evdev 次设备号 */
    if (evdev_minor_counter >= INPUT_MAX_DEVICES) {
        printf("[INPUT] no free evdev minor\n");
        return -1;
    }
    dev->evdev_minor = evdev_minor_counter++;

    /* 加入设备链表头部 */
    dev->next = input_dev_list;
    input_dev_list = dev;

    /* 创建 /dev/input/eventN 字符设备节点 */
    evdev_register_device(dev);

    printf("[INPUT] registered device '%s' -> event%d\n",
           dev->name[0] ? dev->name : "?", dev->evdev_minor);

    return 0;
}

void input_unregister_device(input_dev_t *dev)
{
    if (!dev)
        return;

    /* 从链表中移除 */
    input_dev_t **pp = &input_dev_list;
    while (*pp) {
        if (*pp == dev) {
            *pp = dev->next;
            dev->next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }

    /* 清空对应的帧缓冲区 */
    if (dev->evdev_minor >= 0 && dev->evdev_minor < INPUT_MAX_DEVICES) {
        input_frames[dev->evdev_minor].count = 0;
    }
}

/* ========================================================================
 * 能力管理
 * ======================================================================== */

void input_set_capability(input_dev_t *dev, uint16_t type, uint16_t code)
{
    if (!dev)
        return;

    /* 标记设备支持该事件类型 */
    set_bit(type, dev->evbit);

    /* 在对应的能力位图中标记具体码 */
    switch (type) {
    case EV_KEY:
        set_bit(code, dev->keybit);
        break;
    case EV_REL:
        set_bit(code, dev->relbit);
        break;
    case EV_ABS:
        set_bit(code, dev->absbit);
        break;
    case EV_LED:
        set_bit(code, dev->ledbit);
        break;
    default:
        /* 仅声明事件类型，无具体码位图（如 EV_REP/EV_SND/EV_FF） */
        break;
    }
}

/* ========================================================================
 * 事件上报
 * ======================================================================== */

void input_event(input_dev_t *dev, uint16_t type, uint16_t code, int32_t value)
{
    if (!dev)
        return;
    if (dev->evdev_minor < 0 || dev->evdev_minor >= INPUT_MAX_DEVICES)
        return;

    input_frame_t *frame = &input_frames[dev->evdev_minor];

    /* 帧缓冲区已满时丢弃最旧事件 */
    if (frame->count >= INPUT_FRAME_SIZE) {
        for (int i = 1; i < INPUT_FRAME_SIZE; i++)
            frame->events[i - 1] = frame->events[i];
        frame->count = INPUT_FRAME_SIZE - 1;
    }

    /* 填充时间戳 */
    uint64_t ms = timer_get_ms();
    input_event_t *ev = &frame->events[frame->count];
    ev->tv_sec  = (int64_t)(ms / 1000);
    ev->tv_usec = (int64_t)((ms % 1000) * 1000);
    ev->type    = type;
    ev->code    = code;
    ev->value   = value;
    frame->count++;

    /* 更新设备状态 */
    switch (type) {
    case EV_KEY:
        if (value)
            set_bit(code, dev->key_state);
        else
            clear_bit(code, dev->key_state);
        break;
    case EV_LED:
        if (value)
            set_bit(code, dev->led_state);
        else
            clear_bit(code, dev->led_state);
        break;
    case EV_ABS:
        if (code < ABS_CNT)
            dev->absinfo[code].value = value;
        break;
    default:
        break;
    }
}

void input_report_key(input_dev_t *dev, uint16_t code, int32_t value)
{
    input_event(dev, EV_KEY, code, value);
}

void input_report_rel(input_dev_t *dev, uint16_t code, int32_t value)
{
    input_event(dev, EV_REL, code, value);
}

void input_report_abs(input_dev_t *dev, uint16_t code, int32_t value)
{
    input_event(dev, EV_ABS, code, value);
}

void input_sync(input_dev_t *dev)
{
    if (!dev)
        return;
    if (dev->evdev_minor < 0 || dev->evdev_minor >= INPUT_MAX_DEVICES)
        return;

    input_frame_t *frame = &input_frames[dev->evdev_minor];

    /* 追加 SYN_REPORT 同步事件 */
    if (frame->count < INPUT_FRAME_SIZE) {
        uint64_t ms = timer_get_ms();
        input_event_t *ev = &frame->events[frame->count];
        ev->tv_sec  = (int64_t)(ms / 1000);
        ev->tv_usec = (int64_t)((ms % 1000) * 1000);
        ev->type    = EV_SYN;
        ev->code    = SYN_REPORT;
        ev->value   = 0;
        frame->count++;
    }

    /* 分发整帧给所有 evdev 客户端 */
    evdev_dispatch(dev, frame->events, frame->count);

    /* 清空帧缓冲区，开始下一帧 */
    frame->count = 0;
}

/* ========================================================================
 * 设备列表查询
 * ======================================================================== */

input_dev_t *input_get_device_list(void)
{
    return input_dev_list;
}

input_dev_t *input_get_device(int index)
{
    if (index < 0)
        return NULL;

    input_dev_t *cur = input_dev_list;
    int i = 0;
    while (cur) {
        if (i == index)
            return cur;
        cur = cur->next;
        i++;
    }
    return NULL;
}

/* ========================================================================
 * LED 控制
 * ======================================================================== */

int input_set_led(input_dev_t *dev, uint16_t led, int32_t value)
{
    if (!dev)
        return -1;

    if (value)
        set_bit(led, dev->led_state);
    else
        clear_bit(led, dev->led_state);

    return 0;
}
