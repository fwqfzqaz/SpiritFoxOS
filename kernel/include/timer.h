#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#define PIT_CHANNEL_0    0x40
#define PIT_CHANNEL_1    0x41
#define PIT_CHANNEL_2    0x42
#define PIT_COMMAND      0x43
#define PIT_FREQUENCY    1193182

/* Timer tick rate: 1000 Hz = 1 tick per millisecond */
#define TIMER_HZ         1000
#define PIT_DIVISOR      (PIT_FREQUENCY / TIMER_HZ)

void timer_init(void);
void timer_handler(void);
uint64_t timer_get_ticks(void);
uint64_t timer_get_ms(void);
void timer_sleep_ms(uint64_t ms);

#endif /* TIMER_H */
