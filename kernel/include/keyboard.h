#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

#define KEYBOARD_BUFFER_SIZE 256

/* Extended key codes (returned by keyboard_get_char) */
#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_HOME   0x84
#define KEY_END    0x85
#define KEY_DELETE 0x86
#define KEY_PGUP   0x87
#define KEY_PGDN   0x88
#define KEY_TAB    '\t'

void keyboard_init(void);
void keyboard_handler(void);
char keyboard_get_char(void);
int  keyboard_has_char(void);

#endif
