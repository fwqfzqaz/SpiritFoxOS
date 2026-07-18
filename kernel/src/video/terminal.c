#include "terminal.h"
#include "keyboard.h"
#include "serial.h"
#include "vga.h"
#include "fb.h"
#include "string.h"

/* 终端状态 */
static uint32_t term_flags = TERM_LFLAG_ECHO | TERM_LFLAG_ICANON | TERM_LFLAG_ISIG;

/* 包装函数：根据模式分派到帧缓冲终端或VGA */
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

/* 输入行缓冲区 */
static char term_input_buf[TERM_INPUT_BUF_SIZE];
static int   term_input_len = 0;

/* 输入缓冲区中的光标位置（0到term_input_len） */
static int term_cursor_pos = 0;

/* 已完成行缓冲区（用于规范模式） */
static char term_line_buf[TERM_INPUT_BUF_SIZE];
static int   term_line_len = 0;
static int   term_line_ready = 0;

/* 输出函数 */
static void (*term_output_func)(char) = term_putchar;

/* 特殊按键回调（Tab、上、下） */
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
    /* 从屏幕擦除当前输入 */
    if (term_flags & TERM_LFLAG_ECHO) {
        /* 先将光标移到输入末尾 */
        while (term_cursor_pos < term_input_len) {
            term_putchar(term_input_buf[term_cursor_pos]);
            term_cursor_pos++;
        }
        /* 然后通过退格擦除所有字符 */
        for (int i = 0; i < term_input_len; i++) {
            term_backspace();
        }
    }

    /* 复制新输入 */
    if (len >= TERM_INPUT_BUF_SIZE)
        len = TERM_INPUT_BUF_SIZE - 1;
    if (len > 0) {
        memcpy(term_input_buf, buf, len);
    }
    term_input_buf[len] = '\0';
    term_input_len = len;
    term_cursor_pos = len;

    /* 回显新输入 */
    if (term_flags & TERM_LFLAG_ECHO) {
        for (int i = 0; i < len; i++) {
            term_putchar(term_input_buf[i]);
        }
    }
}

/* 重绘输入行中从 'from' 到末尾的部分，
 * 然后将VGA光标移回term_cursor_pos位置。
 * 用于在中间插入/删除字符后。 */
static void term_redraw_from(int from)
{
    if (!(term_flags & TERM_LFLAG_ECHO))
        return;

    /* 打印从 'from' 到输入末尾的字符 */
    for (int i = from; i < term_input_len; i++) {
        term_putchar(term_input_buf[i]);
    }

    /* 用空格覆盖过期的尾部（如果输入缩短了） -
     * 多打印一个空格来清除旧末尾位置的字符。 */
    term_putchar(' ');

    /* 现在将光标移回term_cursor_pos。
     * 当前VGA位置在 (from + remaining + 1)。
     * 需要回退 (term_input_len - term_cursor_pos + 1) 个字符。 */
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

/* 通过行规程处理输入字符 */
void terminal_input(char c)
{
    /* 转为无符号类型以正确比较扩展按键码（>= 0x80） */
    unsigned char uc = (unsigned char)c;

    /* 信号生成 */
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

    /* 规范（行缓冲）模式处理 */
    if (term_flags & TERM_LFLAG_ICANON) {
        /* 优先处理传递给shell回调的特殊按键 */
        if (uc == TERM_CHAR_UP || uc == TERM_CHAR_DOWN || uc == TERM_CHAR_TAB) {
            if (term_key_cb) {
                term_key_cb(c);
            }
            return;
        }

        /* 左箭头：光标左移 */
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

        /* 右箭头：光标右移 */
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

        /* Home键：光标移到开头 */
        if (uc == TERM_CHAR_HOME) {
            if (term_cursor_pos > 0) {
                term_cursor_pos = 0;
                if (term_flags & TERM_LFLAG_ECHO) {
                    int cx, cy;
                    term_get_cursor(&cx, &cy);
                    /* 向后移动term_input_len个字符 */
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

        /* End键：光标移到输入末尾 */
        if (uc == TERM_CHAR_END) {
            if (term_cursor_pos < term_input_len) {
                if (term_flags & TERM_LFLAG_ECHO) {
                    int cx, cy;
                    term_get_cursor(&cx, &cy);
                    /* 向前移动 (term_input_len - term_cursor_pos) 个字符 */
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

        /* Delete键：删除光标处字符 */
        if (uc == TERM_CHAR_DELETE) {
            if (term_cursor_pos < term_input_len) {
                /* 字符左移 */
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
            /* 回车键：提交当前行 */
            if (term_flags & TERM_LFLAG_ECHO) {
                term_putchar('\n');
            }
            /* 将输入复制到行缓冲区 */
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
            /* 退格键：擦除光标前的字符 */
            if (term_cursor_pos > 0) {
                /* 字符左移 */
                for (int i = term_cursor_pos - 1; i < term_input_len - 1; i++) {
                    term_input_buf[i] = term_input_buf[i + 1];
                }
                term_input_len--;
                term_cursor_pos--;
                term_input_buf[term_input_len] = '\0';

                if (term_flags & TERM_LFLAG_ECHO) {
                    /* 光标后退一位 */
                    int cx, cy;
                    term_get_cursor(&cx, &cy);
                    if (cx > 0) {
                        cx--;
                    } else if (cy > 0) {
                        cy--;
                        cx = 79;
                    }
                    term_set_cursor(cx, cy);
                    /* 从新光标位置重绘到末尾，并清除末尾字符 */
                    term_redraw_from(term_cursor_pos);
                }
            }
            return;
        }

        if (c == TERM_CHAR_KILL) {
            /* Ctrl+U：删除整行 */
            if (term_flags & TERM_LFLAG_ECHO) {
                /* 先将光标移到末尾 */
                while (term_cursor_pos < term_input_len) {
                    term_putchar(term_input_buf[term_cursor_pos]);
                    term_cursor_pos++;
                }
                /* 擦除屏幕上所有字符 */
                for (int i = 0; i < term_input_len; i++) {
                    term_backspace();
                }
            }
            term_input_len = 0;
            term_cursor_pos = 0;
            return;
        }

        if (c == TERM_CHAR_WERASE) {
            /* Ctrl+W：删除上一个单词 */
            /* 跳过末尾空格（光标前） */
            while (term_cursor_pos > 0 && term_input_buf[term_cursor_pos - 1] == ' ') {
                /* 在光标位置左移字符 */
                for (int i = term_cursor_pos - 1; i < term_input_len - 1; i++) {
                    term_input_buf[i] = term_input_buf[i + 1];
                }
                term_input_len--;
                term_cursor_pos--;
                if (term_flags & TERM_LFLAG_ECHO) term_backspace();
            }
            /* 删除单词字符 */
            while (term_cursor_pos > 0 && term_input_buf[term_cursor_pos - 1] != ' ') {
                for (int i = term_cursor_pos - 1; i < term_input_len - 1; i++) {
                    term_input_buf[i] = term_input_buf[i + 1];
                }
                term_input_len--;
                term_cursor_pos--;
                if (term_flags & TERM_LFLAG_ECHO) term_backspace();
            }
            /* 重绘以修复行中删除后的显示 */
            if (term_flags & TERM_LFLAG_ECHO) {
                term_redraw_from(term_cursor_pos);
            }
            return;
        }

        /* 普通字符：在光标位置插入 */
        if (term_input_len < TERM_INPUT_BUF_SIZE - 1) {
            /* 字符右移以腾出空间 */
            for (int i = term_input_len; i > term_cursor_pos; i--) {
                term_input_buf[i] = term_input_buf[i - 1];
            }
            term_input_buf[term_cursor_pos] = c;
            term_input_len++;
            term_input_buf[term_input_len] = '\0';
            term_cursor_pos++;

            if (term_flags & TERM_LFLAG_ECHO) {
                /* 从插入位置重绘 */
                term_redraw_from(term_cursor_pos - 1);
            }
        }
        return;
    }

    /* 原始模式：直接将字符传入行缓冲区 */
    if (term_line_len < TERM_INPUT_BUF_SIZE - 1) {
        term_line_buf[term_line_len++] = c;
    }
    term_line_buf[term_line_len] = '\0';
    term_line_ready = 1;
}

int terminal_readline(char *buf, int maxlen)
{
    /* 等待完整行 — 同时接受键盘和串口输入 */
    while (!term_line_ready) {
        /* 优先检查串口输入（用于 QEMU serial stdio 测试） */
        if (serial_has_char()) {
            char c = serial_get_char();
            /* 将 \r 转为 \n 以兼容终端输入 */
            if (c == '\r') c = '\n';
            terminal_input(c);
        } else if (keyboard_has_char()) {
            char c = keyboard_get_char();
            terminal_input(c);
        } else {
            /* 无输入时让出 CPU */
            __asm__ volatile ("hlt");
        }
    }

    /* 将行复制到调用者的缓冲区 */
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
