#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include "idt.h"

typedef struct {
    int32_t x, y;
    uint8_t buttons;       /* bit0=left, bit1=right, bit2=middle */
    int8_t scroll;
    uint32_t screen_width;
    uint32_t screen_height;
} mouse_state_t;

void mouse_init(uint32_t screen_w, uint32_t screen_h);
void mouse_handler(struct interrupt_frame *frame);
mouse_state_t mouse_get_state(void);

/* USB HID mouse integration */
void usb_mouse_update(int8_t dx, int8_t dy, uint8_t buttons);

#endif /* MOUSE_H */
