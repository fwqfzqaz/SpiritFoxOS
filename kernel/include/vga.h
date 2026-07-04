#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include "boot.h"
#include "console.h"

void vga_init(BootInfo* info);
void vga_putchar(char c);
void vga_print(const char* str);
void vga_clear(void);
void vga_set_color(uint32_t fg, uint32_t bg);
void vga_set_cursor(int x, int y);
void vga_get_cursor(int* x, int* y);
void vga_backspace(void);
void terminal_clear(void);

#endif
