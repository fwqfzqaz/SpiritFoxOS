#include "terminal.h"
#include "keyboard.h"
#include "vga.h"
#include "fb.h"
#include "string.h"

/* Terminal state */
static uint32_t term_flags = TERM_LFLAG_ECHO | TERM_LFLAG_ICANON | TERM_LFLAG_ISIG;

/* Wrapper functions: dispatch to fb terminal or VGA depending on mode */
static void term_putchar(char c)
{
    if (fb_term_is_active())
        fb_term_putchar(c);
    else
        vga_putchar(c);
}

static void term_get_cursor(int *cx, int *cy)
{
    if (fb_term_is_active())
        fb_term_get_cursor(cx, cy);
    else
        vga_get_cursor(cx, cy);
}

static void term_set_cursor(int cx, int cy)
{
    if (fb_term_is_active())
        fb_term_set_cursor(cx, cy);
    else
        vga_set_cursor(cx, cy);
}

static void term_backspace(void)
{
    if (fb_term_is_active())
        fb_term_backspace();
    else
        vga_backspace();
}

/* Input line buffer */
static char term_input_buf[TERM_INPUT_BUF_SIZE];
static int   term_input_len = 0;

/* Cursor position within input buffer (0 to term_input_len) */
static int term_cursor_pos = 0;

/* Completed line buffer (for canonical mode) */
static char term_line_buf[TERM_INPUT_BUF_SIZE];
static int   term_line_len = 0;
static int   term_line_ready = 0;

/* Output function */
static void (*term_output_func)(char) = term_putchar;

/* Key callback for special keys (Tab, Up, Down) */
static term_key_cb_t term_key_cb = NULL;

void terminal_init(void)
{
    term_input_len = 0;
    term_cursor_pos = 0;
    term_line_len = 0;
    term_line_ready = 0;
    term_flags = TERM_LFLAG_ECHO | TERM_LFLAG_ICANON | TERM_LFLAG_ISIG;
    term_key_cb = NULL;
}

void terminal_set_key_callback(term_key_cb_t cb)
{
    term_key_cb = cb;
}

const char* terminal_get_input(void)
{
    return term_input_buf;
}

int terminal_get_input_len(void)
{
    return term_input_len;
}

void terminal_set_input(const char *buf, int len)
{
    /* Erase current input from screen */
    if (term_flags & TERM_LFLAG_ECHO) {
        /* Move cursor to end of input first */
        while (term_cursor_pos < term_input_len) {
            term_putchar(term_input_buf[term_cursor_pos]);
            term_cursor_pos++;
        }
        /* Now erase all characters by backspacing */
        for (int i = 0; i < term_input_len; i++) {
            term_backspace();
        }
    }

    /* Copy new input */
    if (len >= TERM_INPUT_BUF_SIZE)
        len = TERM_INPUT_BUF_SIZE - 1;
    if (len > 0) {
        memcpy(term_input_buf, buf, len);
    }
    term_input_buf[len] = '\0';
    term_input_len = len;
    term_cursor_pos = len;

    /* Echo the new input */
    if (term_flags & TERM_LFLAG_ECHO) {
        for (int i = 0; i < len; i++) {
            term_putchar(term_input_buf[i]);
        }
    }
}

/* Redraw the portion of the input line from 'from' to the end,
 * then move the VGA cursor back to the term_cursor_pos position.
 * This is used after inserting/deleting characters in the middle. */
static void term_redraw_from(int from)
{
    if (!(term_flags & TERM_LFLAG_ECHO))
        return;

    /* Print characters from 'from' to end of input */
    for (int i = from; i < term_input_len; i++) {
        term_putchar(term_input_buf[i]);
    }

    /* Overwrite the stale tail with spaces (if input shrank) -
     * we print one extra space to clear the character that was
     * at the old end position. */
    term_putchar(' ');

    /* Now move cursor back to term_cursor_pos.
     * Current VGA position is at (from + remaining + 1).
     * We need to go back (term_input_len - term_cursor_pos + 1) characters. */
    int back = term_input_len - term_cursor_pos + 1;
    int cx, cy;
    term_get_cursor(&cx, &cy);
    for (int i = 0; i < back; i++) {
        if (cx > 0) {
            cx--;
        } else if (cy > 0) {
            cy--;
            cx = 79;
        }
    }
    term_set_cursor(cx, cy);
}

/* Process an incoming character through the line discipline */
void terminal_input(char c)
{
    /* Cast to unsigned for correct comparison with extended key codes (>= 0x80) */
    unsigned char uc = (unsigned char)c;

    /* Signal generation */
    if (term_flags & TERM_LFLAG_ISIG) {
        if (c == TERM_CHAR_INTR) {
            if (term_flags & TERM_LFLAG_ECHO) {
                printf("^C\n");
            }
            term_input_len = 0;
            term_cursor_pos = 0;
            term_line_ready = 0;
            return;
        }
        if (c == TERM_CHAR_QUIT) {
            if (term_flags & TERM_LFLAG_ECHO) {
                printf("^\\\n");
            }
            term_input_len = 0;
            term_cursor_pos = 0;
            term_line_ready = 0;
            return;
        }
        if (c == TERM_CHAR_SUSP) {
            return;
        }
    }

    /* Canonical (line-buffered) mode processing */
    if (term_flags & TERM_LFLAG_ICANON) {
        /* Handle special keys passed to shell callback first */
        if (uc == TERM_CHAR_UP || uc == TERM_CHAR_DOWN || uc == TERM_CHAR_TAB) {
            if (term_key_cb) {
                term_key_cb(c);
            }
            return;
        }

        /* Left arrow: move cursor left */
        if (uc == TERM_CHAR_LEFT) {
            if (term_cursor_pos > 0) {
                term_cursor_pos--;
                if (term_flags & TERM_LFLAG_ECHO) {
                    int cx, cy;
                    term_get_cursor(&cx, &cy);
                    if (cx > 0) {
                        term_set_cursor(cx - 1, cy);
                    } else if (cy > 0) {
                        term_set_cursor(79, cy - 1);
                    }
                }
            }
            return;
        }

        /* Right arrow: move cursor right */
        if (uc == TERM_CHAR_RIGHT) {
            if (term_cursor_pos < term_input_len) {
                term_cursor_pos++;
                if (term_flags & TERM_LFLAG_ECHO) {
                    int cx, cy;
                    term_get_cursor(&cx, &cy);
                    cx++;
                    if (cx >= 80) {
                        cx = 0;
                        cy++;
                    }
                    term_set_cursor(cx, cy);
                }
            }
            return;
        }

        /* Home: move cursor to beginning */
        if (uc == TERM_CHAR_HOME) {
            if (term_cursor_pos > 0) {
                term_cursor_pos = 0;
                if (term_flags & TERM_LFLAG_ECHO) {
                    int cx, cy;
                    term_get_cursor(&cx, &cy);
                    /* Move back by term_input_len characters */
                    for (int i = 0; i < term_input_len; i++) {
                        if (cx > 0) {
                            cx--;
                        } else if (cy > 0) {
                            cy--;
                            cx = 79;
                        }
                    }
                    term_set_cursor(cx, cy);
                }
            }
            return;
        }

        /* End: move cursor to end of input */
        if (uc == TERM_CHAR_END) {
            if (term_cursor_pos < term_input_len) {
                if (term_flags & TERM_LFLAG_ECHO) {
                    int cx, cy;
                    term_get_cursor(&cx, &cy);
                    /* Move forward by (term_input_len - term_cursor_pos) characters */
                    int fwd = term_input_len - term_cursor_pos;
                    for (int i = 0; i < fwd; i++) {
                        cx++;
                        if (cx >= 80) {
                            cx = 0;
                            cy++;
                        }
                    }
                    term_set_cursor(cx, cy);
                }
                term_cursor_pos = term_input_len;
            }
            return;
        }

        /* Delete: remove character at cursor */
        if (uc == TERM_CHAR_DELETE) {
            if (term_cursor_pos < term_input_len) {
                /* Shift characters left */
                for (int i = term_cursor_pos; i < term_input_len - 1; i++) {
                    term_input_buf[i] = term_input_buf[i + 1];
                }
                term_input_len--;
                term_input_buf[term_input_len] = '\0';
                term_redraw_from(term_cursor_pos);
            }
            return;
        }

        if (c == '\n' || c == '\r') {
            /* Enter: submit the current line */
            if (term_flags & TERM_LFLAG_ECHO) {
                term_putchar('\n');
            }
            /* Copy input to line buffer */
            if (term_input_len > 0) {
                memcpy(term_line_buf, term_input_buf, term_input_len);
            }
            term_line_buf[term_input_len] = '\0';
            term_line_len = term_input_len;
            term_line_ready = 1;
            term_input_len = 0;
            term_cursor_pos = 0;
            return;
        }

        if (c == TERM_CHAR_EOF) {
            if (term_input_len == 0) {
                term_line_buf[0] = '\0';
                term_line_len = 0;
                term_line_ready = 1;
                return;
            }
            memcpy(term_line_buf, term_input_buf, term_input_len);
            term_line_buf[term_input_len] = '\0';
            term_line_len = term_input_len;
            term_line_ready = 1;
            term_input_len = 0;
            term_cursor_pos = 0;
            return;
        }

        if (c == TERM_CHAR_ERASE || c == '\b') {
            /* Backspace: erase character before cursor */
            if (term_cursor_pos > 0) {
                /* Shift characters left */
                for (int i = term_cursor_pos - 1; i < term_input_len - 1; i++) {
                    term_input_buf[i] = term_input_buf[i + 1];
                }
                term_input_len--;
                term_cursor_pos--;
                term_input_buf[term_input_len] = '\0';

                if (term_flags & TERM_LFLAG_ECHO) {
                    /* Move cursor back one */
                    int cx, cy;
                    term_get_cursor(&cx, &cy);
                    if (cx > 0) {
                        cx--;
                    } else if (cy > 0) {
                        cy--;
                        cx = 79;
                    }
                    term_set_cursor(cx, cy);
                    /* Redraw from new cursor pos to end, plus clear trailing char */
                    term_redraw_from(term_cursor_pos);
                }
            }
            return;
        }

        if (c == TERM_CHAR_KILL) {
            /* Ctrl+U: kill entire line */
            if (term_flags & TERM_LFLAG_ECHO) {
                /* Move cursor to end first */
                while (term_cursor_pos < term_input_len) {
                    term_putchar(term_input_buf[term_cursor_pos]);
                    term_cursor_pos++;
                }
                /* Erase all characters on screen */
                for (int i = 0; i < term_input_len; i++) {
                    term_backspace();
                }
            }
            term_input_len = 0;
            term_cursor_pos = 0;
            return;
        }

        if (c == TERM_CHAR_WERASE) {
            /* Ctrl+W: erase last word */
            /* Skip trailing spaces (before cursor) */
            while (term_cursor_pos > 0 && term_input_buf[term_cursor_pos - 1] == ' ') {
                /* Shift characters left at cursor pos */
                for (int i = term_cursor_pos - 1; i < term_input_len - 1; i++) {
                    term_input_buf[i] = term_input_buf[i + 1];
                }
                term_input_len--;
                term_cursor_pos--;
                if (term_flags & TERM_LFLAG_ECHO) term_backspace();
            }
            /* Erase word characters */
            while (term_cursor_pos > 0 && term_input_buf[term_cursor_pos - 1] != ' ') {
                for (int i = term_cursor_pos - 1; i < term_input_len - 1; i++) {
                    term_input_buf[i] = term_input_buf[i + 1];
                }
                term_input_len--;
                term_cursor_pos--;
                if (term_flags & TERM_LFLAG_ECHO) term_backspace();
            }
            /* Redraw to fix display after mid-line deletion */
            if (term_flags & TERM_LFLAG_ECHO) {
                term_redraw_from(term_cursor_pos);
            }
            return;
        }

        /* Regular character: insert at cursor position */
        if (term_input_len < TERM_INPUT_BUF_SIZE - 1) {
            /* Shift characters right to make room */
            for (int i = term_input_len; i > term_cursor_pos; i--) {
                term_input_buf[i] = term_input_buf[i - 1];
            }
            term_input_buf[term_cursor_pos] = c;
            term_input_len++;
            term_input_buf[term_input_len] = '\0';
            term_cursor_pos++;

            if (term_flags & TERM_LFLAG_ECHO) {
                /* Redraw from inserted position */
                term_redraw_from(term_cursor_pos - 1);
            }
        }
        return;
    }

    /* Raw mode: pass character directly to line buffer */
    if (term_line_len < TERM_INPUT_BUF_SIZE - 1) {
        term_line_buf[term_line_len++] = c;
    }
    term_line_buf[term_line_len] = '\0';
    term_line_ready = 1;
}

int terminal_readline(char *buf, int maxlen)
{
    /* Wait for a complete line */
    while (!term_line_ready) {
        char c = keyboard_get_char();
        terminal_input(c);
    }

    /* Copy line to caller's buffer */
    int len = term_line_len < maxlen - 1 ? term_line_len : maxlen - 1;
    if (len > 0) {
        memcpy(buf, term_line_buf, len);
    }
    buf[len] = '\0';

    term_line_ready = 0;
    term_line_len = 0;

    return len;
}

void terminal_write(const char *s, int len)
{
    for (int i = 0; i < len; i++) {
        term_output_func(s[i]);
    }
}

void terminal_putchar(char c)
{
    term_output_func(c);
}

int terminal_line_ready(void)
{
    return term_line_ready;
}

uint32_t terminal_get_flags(void)
{
    return term_flags;
}

void terminal_set_flags(uint32_t flags)
{
    term_flags = flags;
}
