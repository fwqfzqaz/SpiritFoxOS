#include "timer.h"
#include "hal.h"
#include "apic.h"
#include "process.h"
#include "vga.h"
#include "net_tcp.h"
#include "net_icmp.h"

static volatile uint64_t timer_ticks = 0;

/* 定时器中断触发时由 IRQ 处理程序调用（向量 32） */
void timer_handler(void)
{
    timer_ticks++;
    scheduler_tick();

    /* 每 10ms 调用一次网络定时器（约 100Hz） */
    static uint64_t net_tick_counter = 0;
    net_tick_counter++;
    if (net_tick_counter >= 10) {
        net_tick_counter = 0;
        net_tcp_tick();
        net_icmp_tick();
    }
}

void timer_init(void)
{
    /* 配置 8254 PIT：通道 0，低字节/高字节，速率发生器（模式 2） */
    hal_outb(PIT_COMMAND, 0x36);

    /* 设置 TIMER_HZ 频率的除数 */
    uint16_t divisor = PIT_DIVISOR;
    hal_outb(PIT_CHANNEL_0, divisor & 0xFF);        /* 低字节 */
    hal_io_wait();
    hal_outb(PIT_CHANNEL_0, (divisor >> 8) & 0xFF); /* 高字节 */

    printf("[TIMER] PIT initialized at %d Hz (divisor: %d)\n", TIMER_HZ, divisor);
}

uint64_t timer_get_ticks(void)
{
    return timer_ticks;
}

uint64_t timer_get_ms(void)
{
    return timer_ticks;  /* 在 TIMER_HZ=1000 时，1 滴答 = 1 毫秒 */
}

void timer_sleep_ms(uint64_t ms)
{
    uint64_t target = timer_ticks + ms;
    while (timer_ticks < target) {
        hal_halt_no_cli();
    }
}
