#include "timer.h"
#include "hal.h"
#include "apic.h"
#include "process.h"
#include "vga.h"

static volatile uint64_t timer_ticks = 0;

/* Called from IRQ handler when timer interrupt fires (vector 32) */
void timer_handler(void)
{
    timer_ticks++;
    scheduler_tick();
}

void timer_init(void)
{
    /* Configure 8254 PIT: channel 0, lobyte/hibyte, rate generator (mode 2) */
    hal_outb(PIT_COMMAND, 0x36);

    /* Set divisor for TIMER_HZ frequency */
    uint16_t divisor = PIT_DIVISOR;
    hal_outb(PIT_CHANNEL_0, divisor & 0xFF);        /* Low byte */
    hal_io_wait();
    hal_outb(PIT_CHANNEL_0, (divisor >> 8) & 0xFF); /* High byte */

    printf("[TIMER] PIT initialized at %d Hz (divisor: %d)\n", TIMER_HZ, divisor);
}

uint64_t timer_get_ticks(void)
{
    return timer_ticks;
}

uint64_t timer_get_ms(void)
{
    return timer_ticks;  /* 1 tick = 1 ms at TIMER_HZ=1000 */
}

void timer_sleep_ms(uint64_t ms)
{
    uint64_t target = timer_ticks + ms;
    while (timer_ticks < target) {
        hal_halt_no_cli();
    }
}
