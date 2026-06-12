/* SpiritFoxOS - PS/2鼠标驱动
 * Copyright (C) 2025 SpiritFoxOS Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "mouse.h"
#include "pic.h"
#include "../include/io.h"

static mouse_state_t state;
static uint8_t mouse_cycle = 0;
static uint8_t mouse_bytes[4];

/* ---- 底层PS/2辅助函数 ---- */

static void ps2_wait_write(void) {
    uint32_t t = 100000;
    while (--t && (inb(0x64) & 0x02))
        ;
}

static void ps2_wait_read(void) {
    uint32_t t = 100000;
    while (--t && !(inb(0x64) & 0x01))
        ;
}

/* 丢弃输出缓冲区中的所有数据 */
static inline void ps2_drain(void) {
    while (inb(0x64) & 0x01)
        inb(0x60);
}

static inline void ps2_delay(void) {
    for (volatile int i = 0; i < 50000; i++)
        ;
}

/* 向辅助设备（鼠标）发送一个字节，读取响应。
 * 返回响应字节，超时返回0xFF。 */
static uint8_t aux_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(0x64, 0xD4);          /* 写入辅助设备 */
    ps2_wait_write();
    outb(0x60, cmd);            /* 命令字节 */
    ps2_wait_read();             /* 等待响应 */
    return inb(0x60);            /* 读取ACK/响应 */
}

/* 发送命令，遇到RESEND(0xFE)时最多重试3次。 */
static int aux_cmd_ok(uint8_t cmd) {
    for (int i = 0; i < 3; i++) {
        uint8_t r = aux_cmd(cmd);
        if (r == 0xFA) return 1;   /* ACK */
        if (r == 0xFE) continue;   /* RESEND */
        break;                     /* 意外响应 */
    }
    return 0;
}

/* ---- 初始化 ---- */

void mouse_init(uint32_t screen_w, uint32_t screen_h) {
    state.x = (int32_t)(screen_w / 2);
    state.y = (int32_t)(screen_h / 2);
    state.buttons = 0;
    state.scroll = 0;
    state.screen_width = screen_w;
    state.screen_height = screen_h;
    mouse_cycle = 0;

    cli();

    /* 清空输出缓冲区中的陈旧字节 */
    ps2_drain();
    ps2_delay();

    /* 启用PS/2辅助设备（鼠标端口）。
     * 这不会重置控制器或影响键盘端口。 */
    ps2_wait_write();
    outb(0x64, 0xA8);
    ps2_delay();

    /* 读取当前配置，启用IRQ12（位1），保持其他不变。
     * 不要触碰位6（转换）——键盘需要它！ */
    ps2_wait_write();
    outb(0x64, 0x20);            /* 读取配置 */
    ps2_wait_read();
    uint8_t cfg = inb(0x60);

    if (!(cfg & 0x02)) {          /* 仅在IRQ12未设置时才修改 */
        cfg |= 0x02;              /* 启用鼠标中断（IRQ12） */
        ps2_wait_write();
        outb(0x64, 0x60);         /* 写入配置 */
        ps2_wait_write();
        outb(0x60, cfg);
    }

    ps2_delay();
    ps2_drain();

    /* 重置鼠标：发送ACK + BAT OK(0xAA) + 设备ID(0x00) */
    aux_cmd(0xFF);
    ps2_delay();

    /* 消耗BAT结果和设备ID（可能不总是出现） */
    ps2_wait_read();
    inb(0x60);                    /* BAT结果（应该为0xAA） */
    ps2_wait_read();
    inb(0x60);                    /* 设备ID（通常为0x00） */

    ps2_drain();
    ps2_delay();

    /* 设置默认值：清除缩放/分辨率覆盖 */
    aux_cmd_ok(0xF6);
    ps2_delay();
    ps2_drain();

    /* 启用数据报告 — 鼠标开始发送移动数据包 */
    aux_cmd_ok(0xF4);
    ps2_delay();
    ps2_drain();

    sti();

    /* 注册中断处理程序并解屏蔽IRQ12 */
    idt_register_handler(44, mouse_handler);
    pic_unmask_irq(12);
}

/* ---- 中断处理程序 ---- */

void mouse_handler(struct interrupt_frame *frame) {
    (void)frame;

    /*
     * 在真实硬件上，IRQ12对鼠标和有时键盘数据都会触发。
     * 状态寄存器(0x64)的位5告诉我们数据是来自
     * 辅助设备（鼠标）还是键盘端口。
     *
     * QEMU通常不需要此检查，因为它正确路由中断，
     * 但真实的PS/2控制器通常需要它。
     */
    uint8_t status = inb(0x64);
    if (!(status & 0x20)) {
        /* 数据来自键盘而非鼠标 — 丢弃它 */
        (void)inb(0x60);
        pic_send_eoi(12);
        return;
    }

    uint8_t data = inb(0x60);
    mouse_bytes[mouse_cycle] = data;

    if (mouse_cycle == 0) {
        /* 3字节数据包的第一个字节：位[3:0]必须设置位3 */
        if (!(data & 0x08)) {
            /*
             * 不同步 — 丢弃此字节并重置循环计数器。
             *
             * 重要：我们必须将mouse_cycle重置为0（不能保留或递增）。
             * 如果我们不重置，丢失的字节可能导致永久不同步，其中
             * 字节2被当作字节0、字节0被当作字节1等，导致Y
             * 位移被误解释为按钮状态（这是"移动时触发按钮"bug
             * 的根本原因）。
             */
            mouse_cycle = 0;  /* 停留在0，继续寻找有效的数据包起始 */
            pic_send_eoi(12);
            return;
        }
        mouse_cycle++;
    } else if (mouse_cycle == 1) {
        /* 第二个字节：X位移 */
        mouse_cycle++;
    } else {
        /* 第三个字节：Y位移 — 完整数据包已接收 */
        mouse_cycle = 0;

        /*
         * PS/2鼠标数据包格式（3字节）：
         *   字节0: [Y溢出][X溢出][Y符号][X符号][1][中键][右键][左键]
         *   字节1: X位移量（有符号，符号扩展自字节0的位4-5）
         *   字节2: Y位移量（有符号，符号扩展自字节0的位6-7）
         *
         * 按钮状态在字节0的低3位中，而不是在字节2中。
         * 从字节2（Y位移量）读取按钮状态会在每次鼠标移动时导致虚假的
         * 按钮事件——这是"移动时自动点击"bug的根本原因。
         */
        uint8_t new_buttons = mouse_bytes[0] & 0x07;
        if (new_buttons != state.buttons) {
            /*
             * 额外的完整性检查：如果X和Y位移量都为零但按钮发生了变化，
             * 这可能是真实的按钮事件。如果有移动且按钮发生了变化，
             * 验证变化是否合理（不是所有位都在疯狂翻转）。
             */
            int8_t dx_raw = (int8_t)mouse_bytes[1];
            int8_t dy_raw = (int8_t)mouse_bytes[2];

            uint8_t changed = new_buttons ^ state.buttons;
            if (dx_raw != 0 || dy_raw != 0) {
                /*
                 * 移动+按钮变化：使用保守过滤策略。
                 * 仅允许在移动期间的单比特变化，以过滤掉多位同时翻转的
                 * 损坏数据（对于真实鼠标来说这在物理上是不可能的——
                 * 你不可能在同一个采样周期内按下和释放多个按钮）。
                 */
                if ((changed & (changed - 1)) == 0) {
                    /* 单比特变化——可能是合法的 */
                    state.buttons = new_buttons;
                }
                /* 否则：移动期间多比特变化 → 忽略（噪声） */
            } else {
                /* 无移动，仅按钮变化 → 接受（真实事件） */
                state.buttons = new_buttons;
            }
        }
        /* 如果new_buttons == state.buttons，无需更新（优化） */

        int32_t dx = (int32_t)(int8_t)mouse_bytes[1];
        int32_t dy = (int32_t)(int8_t)mouse_bytes[2];
        dy = -dy;                /* PS/2 Y轴与屏幕坐标方向相反 */

        state.x += dx;
        state.y += dy;

        /* 限制在屏幕边界内 */
        if (state.x < 0) state.x = 0;
        if (state.y < 0) state.y = 0;
        if (state.x >= (int32_t)state.screen_width)
            state.x = (int32_t)state.screen_width - 1;
        if (state.y >= (int32_t)state.screen_height)
            state.y = (int32_t)state.screen_height - 1;
    }

    pic_send_eoi(12);
}

/* ---- 公共API ---- */

mouse_state_t mouse_get_state(void) {
    return state;
}

/* 由USB HID鼠标驱动调用，将USB鼠标输入合并到共享状态中 */
void usb_mouse_update(int8_t dx, int8_t dy, uint8_t buttons) {
    state.x += dx;
    state.y += dy;

    /* 合并按钮状态：bit0=左键, bit1=右键, bit2=中键 */
    if (buttons & 0x01) state.buttons |= 0x01; else state.buttons &= ~0x01;
    if (buttons & 0x02) state.buttons |= 0x02; else state.buttons &= ~0x02;
    if (buttons & 0x04) state.buttons |= 0x04; else state.buttons &= ~0x04;

    if (state.x < 0) state.x = 0;
    if (state.y < 0) state.y = 0;
    if (state.x >= (int32_t)state.screen_width)
        state.x = (int32_t)state.screen_width - 1;
    if (state.y >= (int32_t)state.screen_height)
        state.y = (int32_t)state.screen_height - 1;
}
