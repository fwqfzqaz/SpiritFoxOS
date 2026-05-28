#ifndef PIT_H
#define PIT_H

#include <stdint.h>

#define PIT_CHANNEL0  0x40
#define PIT_CHANNEL1  0x41
#define PIT_CHANNEL2  0x42
#define PIT_COMMAND   0x43

#define PIT_FREQ      1193182  /* Base frequency in Hz */

void pit_init(uint32_t freq);
void pit_set_frequency(uint32_t freq);

#endif /* PIT_H */
