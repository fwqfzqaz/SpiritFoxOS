#ifndef GUI_H
#define GUI_H

#include <stdint.h>

void gui_init(uint64_t fb_addr, uint32_t fb_width, uint32_t fb_height,
              uint32_t fb_pitch, uint32_t fb_bpp);
void gui_run(void);

#endif /* GUI_H */
