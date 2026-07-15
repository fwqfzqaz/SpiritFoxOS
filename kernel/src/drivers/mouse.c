/*
 * mouse.c - SpiritFoxOS 的 PS/2 鼠标驱动
 *
 * 使用 8042 键盘控制器的辅助设备端口。
 * IRQ12（向量 44）每字节触发一次；解码 3 字节数据包。
 *
 * 关键设计要点：
 *   - 端口 0x60 由键盘和鼠标共享。我们检查
 *     KBC 状态寄存器位 5 以确认数据来自鼠标。
 *   - 包同步通过始终寻找有效的字节 0（位 3 置位，
 *     位 6-7 清零）来维护，然后才接受后续字节。
 */

#include "mouse.h"
#include "hal.h"
#include "vga.h"

/* ---- 8042 键盘控制器端口 ---- */
#define KBC_CMD    0x64
#define KBC_DATA   0x60
#define KBC_STATUS 0x64

/* KBC 状态寄存器位 */
#define KBC_STS_OBF   0x01   /* 输出缓冲区满 */
#define KBC_STS_AUX   0x20   /* 数据来自辅助（鼠标）设备 */

/* ---- 内部状态 ---- */
static int      mouse_x;
static int      mouse_y;
static uint8_t  mouse_buttons;

static volatile int mouse_event_pending;

/* ---- 3 字节包重组 ---- */
static uint8_t  pkt_buf[3];
static int      pkt_idx;

/* ---- 辅助函数 ---- */

static void kbc_wait_write(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(hal_inb(KBC_STATUS) & 0x02))
            return;
    }
}

static void kbc_wait_read(void)
{
    for (int i = 0; i < 100000; i++) {
        if (hal_inb(KBC_STATUS) & KBC_STS_OBF)
            return;
    }
}

/* 检查输出缓冲区中的下一个字节是否来自鼠标 */
static int kbc_is_aux_data(void)
{
    return (hal_inb(KBC_STATUS) & KBC_STS_AUX) != 0;
}

/* 通过 KBC 辅助端口向鼠标发送命令 */
static int mouse_cmd(uint8_t cmd)
{
    uint8_t ack;

    for (int retry = 0; retry < 3; retry++) {
        kbc_wait_write();
        hal_outb(KBC_CMD, 0xD4);         /* 将下一字节路由到鼠标 */
        kbc_wait_write();
        hal_outb(KBC_DATA, cmd);

        /* 等待鼠标的 ACK（0xFA） */
        for (int i = 0; i < 100000; i++) {
            uint8_t sts = hal_inb(KBC_STATUS);
            if (sts & KBC_STS_OBF) {
                if (sts & KBC_STS_AUX) {
                    ack = hal_inb(KBC_DATA);
                    if (ack == 0xFA)
                        return 0;        /* 成功 */
                    if (ack == 0xFE)
                        break;           /* 请求重发，重试 */
                } else {
                    /* 键盘数据到达 - 丢弃 */
                    hal_inb(KBC_DATA);
                }
            }
        }
    }
    return -1;  /* 重试后失败 */
}

/* ---- 检查字节是否为有效的 PS/2 包首字节 ---- */
static int is_valid_first_byte(uint8_t b)
{
    /* 位 3 必须置位（有效包中始终为 1） */
    if (!(b & 0x08))
        return 0;
    /* 位 6-7 应为 0（保留，标准 PS/2 中始终为 0） */
    if (b & 0xC0)
        return 0;
    return 1;
}

/* ---- 公共接口：初始化 PS/2 鼠标 ---- */

void mouse_init(void)
{
    pkt_idx = 0;
    mouse_x = 512;
    mouse_y = 384;
    mouse_buttons = 0;
    mouse_event_pending = 0;

    /* 1. 设置期间禁用设备以避免虚假数据 */
    kbc_wait_write();
    hal_outb(KBC_CMD, 0xA7);          /* 禁用辅助（鼠标）端口 */
    kbc_wait_write();
    hal_outb(KBC_CMD, 0xAD);          /* 禁用键盘端口 */

    /* 2. 清空输出缓冲区中的待处理数据 */
    while (hal_inb(KBC_STATUS) & KBC_STS_OBF)
        hal_inb(KBC_DATA);

    /* 3. 使能辅助设备（鼠标端口） */
    kbc_wait_write();
    hal_outb(KBC_CMD, 0xA8);          /* 使能辅助端口 */

    /* 4. 配置 KBC 命令字节：使能两个 IRQ，使能键盘 */
    kbc_wait_write();
    hal_outb(KBC_CMD, 0x60);
    kbc_wait_write();
    hal_outb(KBC_DATA, 0x47);         /* 位0=键盘中断, 位1=鼠标中断, 位6=扫描码转换 */

    /* 5. 重置鼠标 */
    if (mouse_cmd(0xFF) != 0) {
        printf("[MOUSE] Reset failed, retrying...\n");
        mouse_cmd(0xFF);
    }

    /* 读取鼠标自检结果（0xAA）和鼠标 ID（0x00） */
    kbc_wait_read();
    if (kbc_is_aux_data())
        hal_inb(KBC_DATA);            /* 0xAA = 自检通过 */
    kbc_wait_read();
    if (kbc_is_aux_data())
        hal_inb(KBC_DATA);            /* 0x00 = 标准鼠标 ID */

    /* 6. 设置采样率（100 采样/秒 - 默认值） */
    mouse_cmd(0xF3);                  /* 设置采样率 */
    mouse_cmd(100);                   /* 100 Hz */

    /* 7. 设置分辨率（4 计数/毫米 - 默认值） */
    mouse_cmd(0xE8);                  /* 设置分辨率 */
    mouse_cmd(0x02);                  /* 4 计数/毫米 */

    /* 8. 设置 1:1 缩放 */
    mouse_cmd(0xE6);

    /* 9. 使能数据报告（流模式） */
    mouse_cmd(0xF4);

    /* 10. 重新使能键盘端口 */
    kbc_wait_write();
    hal_outb(KBC_CMD, 0xAE);          /* 使能键盘端口 */

    printf("[MOUSE] PS/2 mouse initialized\n");
}

/* ---- 公共接口：IRQ12 处理程序，由 irq_handler 调用 ---- */

void mouse_handler(void)
{
    /* 验证此数据来自鼠标（而非键盘） */
    uint8_t sts = hal_inb(KBC_STATUS);
    if (!(sts & KBC_STS_OBF))
        return;                        /* 虚假中断 */
    if (!(sts & KBC_STS_AUX)) {
        /* 数据来自键盘而非鼠标 - 交给键盘处理程序处理 */
        /* 读取并丢弃以清除中断，但这种情况通常不应发生 */
        hal_inb(KBC_DATA);
        return;
    }

    uint8_t data = hal_inb(KBC_DATA);

    /* 包同步：
     * 如果我们期望字节 0，检查此字节是否为有效的首字节。
     * 如果不是，持续丢弃直到找到有效的首字节。
     * 如果我们在字节 1 或 2 处遇到看起来像字节 0 的数据，
     * 说明我们丢失了字节 - 重新同步。 */
    if (pkt_idx == 0) {
        if (!is_valid_first_byte(data)) {
            /* 非有效首字节 - 跳过并保持在索引 0 */
            return;
        }
    } else {
        /* 我们在字节 1 或 2 处 - 但如果此字节看起来像新的首字节，
         * 说明上一个数据包不完整。重新开始。 */
        if (is_valid_first_byte(data)) {
            pkt_buf[0] = data;
            pkt_idx = 1;
            return;
        }
    }

    pkt_buf[pkt_idx++] = data;

    if (pkt_idx < 3)
        return;

    /* 收到完整数据包 */
    pkt_idx = 0;

    /* 解码按钮状态 */
    mouse_buttons = pkt_buf[0] & 0x07;

    /* 解码 X/Y 移动（9 位有符号数）：
     *   低 8 位在字节 1/2 中，符号位在字节 0 的位 4/5 中。
     *   不能使用 (int8_t) 强制转换 — 那会使用移动字节的
     *   位 7 作为符号位，这是错误的。符号来自字节 0。 */
    int dx = (int)pkt_buf[1];
    int dy = (int)pkt_buf[2];
    if (pkt_buf[0] & 0x10) dx -= 256;  /* X 为负 */
    if (pkt_buf[0] & 0x20) dy -= 256;  /* Y 为负 */
    dy = -dy;   /* PS/2 Y 轴方向反转（上 = 负值） */

    /* 速度倍数，用于舒适的光标移动 */
    dx *= 2;
    dy *= 2;

    /* 更新绝对位置 */
    mouse_x += dx;
    mouse_y += dy;

    /* 限制在屏幕范围内 */
    if (mouse_x < 0)   mouse_x = 0;
    if (mouse_x > 1023) mouse_x = 1023;
    if (mouse_y < 0)   mouse_y = 0;
    if (mouse_y > 767) mouse_y = 767;

    mouse_event_pending = 1;
}

/* ---- 公共接口：轮询接口 ---- */

int mouse_has_event(void)
{
    return mouse_event_pending;
}

void mouse_get_state(int *x, int *y, uint8_t *buttons)
{
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (buttons) *buttons = mouse_buttons;
    mouse_event_pending = 0;
}

void mouse_get_delta(int *dx, int *dy, uint8_t *buttons)
{
    if (dx) *dx = mouse_x;
    if (dy) *dy = mouse_y;
    if (buttons) *buttons = mouse_buttons;
    mouse_event_pending = 0;
}
