#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/* Mouse button flags */
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

void mouse_init(void);
void mouse_handler(void);

/* Poll-style interface for GUI */
int  mouse_has_event(void);
void mouse_get_state(int *x, int *y, uint8_t *buttons);
void mouse_get_delta(int *dx, int *dy, uint8_t *buttons);

#endif /* MOUSE_H */
