#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>

/* Line discipline flags */
#define TERM_LFLAG_ECHO    0x01   /* Echo input characters */
#define TERM_LFLAG_ICANON  0x02   /* Canonical mode (line-buffered) */
#define TERM_LFLAG_ISIG    0x04   /* Signal generation (Ctrl+C etc.) */

/* Special characters */
#define TERM_CHAR_EOF      0x04   /* Ctrl+D: end of input */
#define TERM_CHAR_INTR     0x03   /* Ctrl+C: interrupt */
#define TERM_CHAR_QUIT     0x1C   /* Ctrl+\: quit */
#define TERM_CHAR_SUSP    0x1A   /* Ctrl+Z: suspend */
#define TERM_CHAR_ERASE   0x7F   /* Backspace: erase character */
#define TERM_CHAR_KILL     0x15   /* Ctrl+U: kill line */
#define TERM_CHAR_WERASE  0x17   /* Ctrl+W: erase word */

/* Extended key codes (from keyboard driver) */
#define TERM_CHAR_UP      0x80
#define TERM_CHAR_DOWN    0x81
#define TERM_CHAR_LEFT    0x82
#define TERM_CHAR_RIGHT   0x83
#define TERM_CHAR_HOME    0x84
#define TERM_CHAR_END     0x85
#define TERM_CHAR_DELETE  0x86
#define TERM_CHAR_TAB     '\t'

/* Terminal input buffer */
#define TERM_INPUT_BUF_SIZE 256

/* Callback for special keys (Tab, Up, Down) - registered by shell */
typedef void (*term_key_cb_t)(char key);
void terminal_set_key_callback(term_key_cb_t cb);

/* Get current input buffer state (for shell editing) */
const char* terminal_get_input(void);
int terminal_get_input_len(void);
void terminal_set_input(const char *buf, int len);

/* Initialize terminal subsystem */
void terminal_init(void);

/* Process a character from keyboard input */
void terminal_input(char c);

/* Read a line from terminal (blocking, returns line length) */
int terminal_readline(char *buf, int maxlen);

/* Write a string to terminal output */
void terminal_write(const char *s, int len);

/* Write a single character to terminal output */
void terminal_putchar(char c);

/* Check if a line is available for reading */
int terminal_line_ready(void);

/* Get current line discipline flags */
uint32_t terminal_get_flags(void);

/* Set line discipline flags */
void terminal_set_flags(uint32_t flags);

#endif /* TERMINAL_H */
