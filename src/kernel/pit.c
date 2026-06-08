#include "pit.h"
#include "../include/io.h"

void pit_init(uint32_t freq) {
    pit_set_frequency(freq);
}

void pit_set_frequency(uint32_t freq) {
    uint16_t divisor = (uint16_t)(PIT_FREQ / freq);

    /* Channel 0, lobyte/hibyte, mode 3 (square wave) */
    outb(PIT_COMMAND, 0x36);

    /* Set divisor */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}
