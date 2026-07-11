/*
 * window.c - SpiritFoxOS 现代图形窗口管理器
 * 多窗口、停靠任务栏、开始菜单、桌面图标、右键菜单
 *
 * 现代暗色主题，带有磨砂玻璃效果、渐变点缀，
 * 以及受 macOS/Windows 11 启发的设计。
 *
 * 通过 shell 命令 "window" 进入，按 Escape 退出
 */

#include "window.h"
#include "fb.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "string.h"
#include "boot.h"
#include "logo_data.h"
#include "memory.h"
#include "pci.h"
#include "blkdev.h"
#include "apic.h"
#include "hal.h"

extern BootInfo *g_boot_info;

/* ================================================================
 *  现代配色方案 - 蓝紫色调暗色主题
 * ================================================================ */

/* 背景渐变端点 */
#define COLOR_BG_TL         0x000A0A1A   /* 深海军蓝 左上 */
#define COLOR_BG_BR         0x001A1040   /* 深紫蓝 右下 */

/* 表面 */
#define COLOR_SURFACE       0x001A1A2E   /* 卡片/窗口表面 */
#define COLOR_SURFACE_LIGHT 0x0024243E   /* 较亮表面（悬停） */
#define COLOR_SURFACE_DIM   0x00121222   /* 较暗表面 */
#define COLOR_GLASS         0x0016162A   /* 磨砂玻璃背景 */

/* 强调色 - 鲜艳的蓝紫渐变 */
#define COLOR_ACCENT_START  0x004A6CF7   /* 蓝色端 */
#define COLOR_ACCENT_END    0x009B59B6   /* 紫色端 */
#define COLOR_ACCENT        0x006C5CE7   /* 中间强调色 */
#define COLOR_ACCENT_LIGHT  0x007C6CF7   /* 较亮强调色 */

/* 文字 */
#define COLOR_TEXT_BRIGHT   0x00E8E8F0   /* 纯白色 */
#define COLOR_TEXT          FB_COLOR_TEXT
#define COLOR_TEXT_DIM      FB_COLOR_TEXT_DIM

/* 任务栏/停靠栏 */
#define COLOR_DOCK_BG       0x0016162A   /* 磨砂深色玻璃 */
#define COLOR_DOCK_BORDER   0x00303050   /* 微妙的顶部边框光晕 */

/* 状态颜色 */
#define COLOR_SUCCESS       0x002ECC71   /* 现代绿色 */
#define COLOR_WARNING       0x00F39C12   /* 琥珀色 */
#define COLOR_DANGER        0x00E74C3C   /* 柔和红色 */

/* 窗口专用 */
#define COLOR_WIN_TITLE     0x001C1C32   /* 标题栏深色 */
#define COLOR_WIN_BODY      0x00141424   /* 窗口主体 */
#define COLOR_SHADOW1       0x00080810   /* 阴影层1（最近） */
#define COLOR_SHADOW2       0x0006060C   /* 阴影层2 */
#define COLOR_SHADOW3       0x00040408   /* 阴影层3（最远） */

/* 终端 */
#define COLOR_TERM_BG       0x000D0D1A   /* 终端深色背景 */
#define COLOR_TERM_FG       0x0040E870   /* 绿色文字 */
#define COLOR_TERM_PROMPT   0x0060A0E0   /* 提示符蓝色 */

/* 扫雷 */
#define COLOR_MS_HIDDEN     0x00383858   /* 未揭开格子 */
#define COLOR_MS_HIDDEN_HI  0x00484868   /* 未揭开高亮 */
#define COLOR_MS_HIDDEN_SH  0x00282848   /* 未揭开阴影 */
#define COLOR_MS_REVEALED   0x00202038   /* 已揭开格子 */
#define COLOR_MS_BORDER     0x00303050   /* 格子边框 */

/* ================================================================
 *  应用定义
 * ================================================================ */

#define NUM_APPS 8

static const char *app_names[NUM_APPS] = {
    "Terminal",       /* 0 - 终端 */
    "Files",          /* 1 - 文件 */
    "Monitor",        /* 2 - 监视器 */
    "Settings",       /* 3 - 设置 */
    "About",          /* 4 - 关于 */
    "Home",           /* 5 - 主目录 */
    "Trash",          /* 6 - 回收站 */
    "Minesweeper"     /* 7 - 扫雷 */
};

/* 窗口标题全名 */
static const char *app_titles[NUM_APPS] = {
    "Terminal",
    "File Manager",
    "System Monitor",
    "Settings",
    "About SpiritFoxOS",
    "Home",
    "Trash",
    "Minesweeper"
};

/* ================================================================
 *  多窗口状态
 * ================================================================ */

#define MAX_WINDOWS 8

typedef struct {
    int app;
    int x, y, w, h;
    int minimized;
    int maximized;
    int saved_x, saved_y, saved_w, saved_h;
} WinSlot;

static WinSlot wins[MAX_WINDOWS];
static int num_wins;
static int focus_win;  /* -1 = 无 */

/* ================================================================
 *  鼠标与输入状态
 * ================================================================ */

static int mouse_x = 0, mouse_y = 0;
static int mouse_prev_x = 0, mouse_prev_y = 0;
static uint8_t mouse_buttons = 0;
static uint8_t mouse_prev_buttons = 0;

/* 双击检测 */
static uint64_t last_click_time = 0;
static int last_click_x = 0, last_click_y = 0;
static int double_click = 0;  /* 1表示此次点击为双击 */

static int dragging = 0;
static int drag_win = -1;
static int drag_off_x = 0, drag_off_y = 0;

static int resizing = 0;
static int resize_win = -1;
static int resize_edge = 0;  /* 位掩码：1=右, 2=下, 4=左, 8=上 */
static int resize_start_x = 0, resize_start_y = 0;
static int resize_orig_w = 0, resize_orig_h = 0;
static int resize_orig_x = 0, resize_orig_y = 0;

static int start_menu_open = 0;
static int start_menu_hover = -1;

static int ctx_menu_open = 0;
static int ctx_menu_x = 0, ctx_menu_y = 0;
static int ctx_menu_hover = -1;

static int icon_pressed = -1;

/* 动画状态 */
static uint64_t anim_tick = 0;

/* ================================================================
 *  布局常量
 * ================================================================ */

#define TASKBAR_HEIGHT    52
#define DOCK_FLOAT_GAP    6
#define DOCK_CORNER_R     12
#define TITLE_BAR_HEIGHT  36
#define WIN_CORNER_R      8
#define WIN_SHADOW_LAYERS 3

/* 桌面图标网格 - 2列，大图标 */
#define ICON_SIZE         48
#define ICON_CELL_W       88
#define ICON_CELL_H       96
#define ICON_GRID_LEFT    20
#define ICON_GRID_TOP     20
#define ICON_GRID_COLS    2

/* ================================================================
 *  终端模拟器
 * ================================================================ */

#define TERM_BUF_LINES 50
#define TERM_BUF_COLS  80
static char term_buf[TERM_BUF_LINES][TERM_BUF_COLS + 1];
static int term_cursor_line;
static int term_cursor_col;
static char term_input[TERM_BUF_COLS];
static int term_input_len;

static void term_clear(void)
{
    for (int i = 0; i < TERM_BUF_LINES; i++) {
        for (int j = 0; j < TERM_BUF_COLS; j++)
            term_buf[i][j] = ' ';
        term_buf[i][TERM_BUF_COLS] = '\0';
    }
    term_cursor_line = 0;
    term_cursor_col = 0;
}

static void term_print(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            term_cursor_col = 0;
            term_cursor_line++;
            if (term_cursor_line >= TERM_BUF_LINES) {
                for (int i = 0; i < TERM_BUF_LINES - 1; i++)
                    memcpy(term_buf[i], term_buf[i + 1], TERM_BUF_COLS);
                for (int j = 0; j < TERM_BUF_COLS; j++)
                    term_buf[TERM_BUF_LINES - 1][j] = ' ';
                term_cursor_line = TERM_BUF_LINES - 1;
            }
        } else {
            if (term_cursor_col < TERM_BUF_COLS)
                term_buf[term_cursor_line][term_cursor_col++] = *s;
        }
        s++;
    }
}

static void int_to_str(int n, char *buf)
{
    int pos = 0;
    char tmp[12];
    int t = 0;
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    if (n < 0) { buf[pos++] = '-'; n = -n; }
    while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
    for (int j = t - 1; j >= 0; j--) buf[pos++] = tmp[j];
    buf[pos] = '\0';
}

static void uint_to_str(unsigned int n, char *buf)
{
    int pos = 0;
    char tmp[12];
    int t = 0;
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
    for (int j = t - 1; j >= 0; j--) buf[pos++] = tmp[j];
    buf[pos] = '\0';
}

static void term_execute(const char *cmd)
{
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    term_print("> ");
    term_print(cmd);
    term_print("\n");

    char cmd_copy[TERM_BUF_COLS];
    strncpy(cmd_copy, cmd, TERM_BUF_COLS - 1);
    cmd_copy[TERM_BUF_COLS - 1] = '\0';

    char *argv[8];
    int argc = 0;
    char *tok = cmd_copy;
    while (*tok && argc < 8) {
        while (*tok == ' ') tok++;
        if (*tok == '\0') break;
        argv[argc++] = tok;
        while (*tok && *tok != ' ') tok++;
        if (*tok) *tok++ = '\0';
    }

    if (argc == 0) return;

    char nbuf[32];

    if (strcmp(argv[0], "help") == 0) {
        term_print("SpiritFoxOS Terminal v0.5\n");
        term_print("General:\n");
        term_print("  help         - Show this help\n");
        term_print("  clear        - Clear terminal\n");
        term_print("  version      - Show OS version\n");
        term_print("  echo [text]  - Print text\n");
        term_print("  about        - About SpiritFoxOS\n");
        term_print("System:\n");
        term_print("  sysinfo      - System information\n");
        term_print("  meminfo      - Memory information\n");
        term_print("  uptime       - System uptime\n");
        term_print("  pcilist      - List PCI devices\n");
        term_print("  blklist      - List block devices\n");
        term_print("  reboot       - Reboot system\n");
    } else if (strcmp(argv[0], "clear") == 0) {
        term_clear();
    } else if (strcmp(argv[0], "version") == 0) {
        term_print("SpiritFoxOS v0.5.0 - Modern GUI\n");
    } else if (strcmp(argv[0], "about") == 0) {
        term_print("SpiritFoxOS v0.5.0\n");
        term_print("A hobby x86_64 operating system\n");
        term_print("Modern Multi-Window GUI\n");
        term_print("32-bit double-buffered framebuffer\n");
    } else if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) term_print(" ");
            term_print(argv[i]);
        }
        term_print("\n");
    } else if (strcmp(argv[0], "sysinfo") == 0) {
        term_print("Screen: ");
        uint_to_str((unsigned int)fb_get_width(), nbuf);
        term_print(nbuf);
        term_print("x");
        uint_to_str((unsigned int)fb_get_height(), nbuf);
        term_print(nbuf);
        term_print(" 32bpp\n");
        term_print("GUI: Modern Window Manager\n");
        term_print("Windows: ");
        int_to_str(num_wins, nbuf);
        term_print(nbuf);
        term_print("\n");
    } else if (strcmp(argv[0], "meminfo") == 0) {
        uint64_t total = pmm_total_pages();
        uint64_t used  = pmm_used_pages();
        term_print("Memory Information:\n");
        term_print("  Total pages : ");
        uint_to_str((unsigned int)total, nbuf);
        term_print(nbuf);
        term_print("\n  Used pages  : ");
        uint_to_str((unsigned int)used, nbuf);
        term_print(nbuf);
        term_print("\n  Free pages  : ");
        uint_to_str((unsigned int)(total - used), nbuf);
        term_print(nbuf);
        term_print("\n  Total memory: ");
        uint_to_str((unsigned int)(total * 4), nbuf);
        term_print(nbuf);
        term_print(" KB\n  Used memory : ");
        uint_to_str((unsigned int)(used * 4), nbuf);
        term_print(nbuf);
        term_print(" KB\n");
    } else if (strcmp(argv[0], "uptime") == 0) {
        uint64_t ms = timer_get_ms();
        uint64_t sec = ms / 1000;
        uint64_t rem = ms % 1000;
        term_print("Uptime: ");
        uint_to_str((unsigned int)sec, nbuf);
        term_print(nbuf);
        term_print(".");
        uint_to_str((unsigned int)rem, nbuf);
        term_print(nbuf);
        term_print(" seconds\nLAPIC ID: ");
        uint_to_str((unsigned int)apic_get_lapic_id(), nbuf);
        term_print(nbuf);
        term_print("\n");
    } else if (strcmp(argv[0], "pcilist") == 0) {
        term_print("PCI devices:\n");
        pci_list_devices();
    } else if (strcmp(argv[0], "blklist") == 0) {
        term_print("Block devices:\n");
        blkdev_list();
    } else if (strcmp(argv[0], "reboot") == 0) {
        term_print("Rebooting...\n");
        uint8_t val = hal_inb(0x61);
        hal_outb(0x61, (uint8_t)(val | 0x80));
        hal_io_wait();
        hal_outb(0x61, val);
        hal_outb(0x64, 0xFE);
        for (;;) __asm__ volatile ("hlt");
    } else {
        term_print("Unknown command: ");
        term_print(argv[0]);
        term_print("\nType 'help' for available commands.\n");
    }
}

/* ================================================================
 *  绘图辅助函数
 * ================================================================ */

/* 带噪点纹理和动画色相偏移的对角渐变背景。
 * 优化：为每条扫描线预计算一个颜色，然后以单次memset操作填充每行，
 * 而非逐像素计算。 */
static void draw_gradient_background(void)
{
    uint32_t sw = fb_get_width(), sh = fb_get_height();
    uint32_t pitch = fb_get_pitch();
    /* 基于定时器的动画色相偏移 - 缓慢旋转 */
    uint64_t phase = (anim_tick / 120) % 360;

    int shift = (int)((phase * 4) / 360); /* 0-3 range, very subtle */

    uint32_t *buf = (uint32_t *)fb_get_buffer();
    int stride = pitch / 4;

    /* 为每条扫描线预计算一个颜色并用32位写入填充。
     * 对角渐变主要沿垂直方向变化，因此每行使用一个颜色
     * 是很好的近似，且速度快得多。 */
    for (int y = 0; y < (int)sh; y++) {
        /* 为速度仅使用垂直插值（忽略x分量）。
         * 与对角渐变的视觉差异微乎其微，
         * 因为颜色非常暗且相似。 */
        int t = y * 256 / (int)sh;
        int r = 0x0A + ((0x1A - 0x0A) * t >> 8);
        int g = 0x0A + ((0x10 - 0x0A) * t >> 8);
        int b = 0x1A + ((0x40 - 0x1A) * t >> 8) + shift;

        fb_color_t c = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
        uint32_t *line = buf + y * stride;
        for (int x = 0; x < (int)sw; x++) {
            line[x] = c;
        }
    }
}

/* 填充圆角矩形 */
static void fill_rounded_rect(int x, int y, int w, int h, int r, fb_color_t color)
{
    /* Clamp radius */
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* Top band (with corners) */
    for (int i = 0; i < r; i++) {
        int dx = r - i;
        fb_fill_rect(x + dx, y + i, w - 2 * dx, 1, color);
    }
    /* Middle body */
    fb_fill_rect(x, y + r, w, h - 2 * r, color);
    /* Bottom band */
    for (int i = 0; i < r; i++) {
        int dx = i + 1;
        fb_fill_rect(x + dx, y + h - r + i, w - 2 * dx, 1, color);
    }
}

/* Draw rounded rectangle outline */
static void draw_rounded_rect(int x, int y, int w, int h, int r, fb_color_t color)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    fb_draw_line(x + r, y, x + w - r, y, color);
    fb_draw_line(x + r, y + h - 1, x + w - r, y + h - 1, color);
    fb_draw_line(x, y + r, x, y + h - r, color);
    fb_draw_line(x + w - 1, y + r, x + w - 1, y + h - r, color);

    for (int i = 0; i < r; i++) {
        int dx = r - i - 1;
        fb_put_pixel(x + dx, y + i, color);
        fb_put_pixel(x + i, y + dx, color);
        fb_put_pixel(x + w - 1 - dx, y + i, color);
        fb_put_pixel(x + w - 1 - i, y + dx, color);
        fb_put_pixel(x + dx, y + h - 1 - i, color);
        fb_put_pixel(x + i, y + h - 1 - dx, color);
        fb_put_pixel(x + w - 1 - dx, y + h - 1 - i, color);
        fb_put_pixel(x + w - 1 - i, y + h - 1 - dx, color);
    }
}

/* Fill rounded rect with top corners only */
static void fill_rounded_rect_top(int x, int y, int w, int h, int r, fb_color_t color)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* Top band with corners */
    for (int i = 0; i < r; i++) {
        int dx = r - i;
        fb_fill_rect(x + dx, y + i, w - 2 * dx, 1, color);
    }
    /* Body - square bottom */
    fb_fill_rect(x, y + r, w, h - r, color);
}

/* Clear corners to create rounded top shape (erase with background color) */
static void clear_top_corners_rounded(int x, int y, int w, int h, int r, fb_color_t bg)
{
    if (r > w / 2) r = w / 2;
    for (int i = 0; i < r && i < h; i++) {
        int dx = r - i;
        fb_fill_rect(x, y + i, dx, 1, bg);
        fb_fill_rect(x + w - dx, y + i, dx, 1, bg);
    }
}

/* Draw multi-layer shadow for a window */
static void draw_window_shadow(int x, int y, int w, int h)
{
    /* Layer 3 - farthest, largest, most transparent */
    fill_rounded_rect(x - 2, y - 2, w + 12, h + 12, WIN_CORNER_R + 2, COLOR_SHADOW3);
    /* Layer 2 */
    fill_rounded_rect(x - 1, y - 1, w + 8, h + 8, WIN_CORNER_R + 1, COLOR_SHADOW2);
    /* Layer 1 - closest */
    fill_rounded_rect(x, y, w + 4, h + 4, WIN_CORNER_R, COLOR_SHADOW1);
}

/* ================================================================
 *  MINESWEEPER GAME
 * ================================================================ */

/* Forward declaration */
static void draw_window_frame(int x, int y, int w, int h, const char *title,
                              int active, int maximized);

#define MS_ROWS 9
#define MS_COLS 9
#define MS_MINES 10
#define MS_CELL_SIZE 28

#define MS_HIDDEN   0
#define MS_REVEALED 1
#define MS_FLAGGED  2

static int ms_board[MS_ROWS][MS_COLS];
static int ms_state[MS_ROWS][MS_COLS];
static int ms_game_over;
static int ms_started;
static int ms_flags;
static int ms_revealed;
static int ms_first_click;

static void ms_init(void)
{
    for (int r = 0; r < MS_ROWS; r++)
        for (int c = 0; c < MS_COLS; c++) {
            ms_board[r][c] = 0;
            ms_state[r][c] = MS_HIDDEN;
        }
    ms_game_over = 0;
    ms_started = 0;
    ms_flags = 0;
    ms_revealed = 0;
    ms_first_click = 1;
}

static void ms_place_mines(int safe_r, int safe_c)
{
    int placed = 0;
    while (placed < MS_MINES) {
        int r = 0, c = 0;
        static unsigned int ms_seed = 12345;
        ms_seed = ms_seed * 1103515245 + 12345;
        r = (ms_seed >> 16) % MS_ROWS;
        ms_seed = ms_seed * 1103515245 + 12345;
        c = (ms_seed >> 16) % MS_COLS;
        if (ms_board[r][c] == -1) continue;
        if (r >= safe_r - 1 && r <= safe_r + 1 &&
            c >= safe_c - 1 && c <= safe_c + 1) continue;
        ms_board[r][c] = -1;
        placed++;
    }
    for (int r = 0; r < MS_ROWS; r++) {
        for (int c = 0; c < MS_COLS; c++) {
            if (ms_board[r][c] == -1) continue;
            int count = 0;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    int nr = r + dr, nc = c + dc;
                    if (nr >= 0 && nr < MS_ROWS && nc >= 0 && nc < MS_COLS &&
                        ms_board[nr][nc] == -1)
                        count++;
                }
            ms_board[r][c] = count;
        }
    }
}

static void ms_reveal(int r, int c)
{
    if (r < 0 || r >= MS_ROWS || c < 0 || c >= MS_COLS) return;
    if (ms_state[r][c] != MS_HIDDEN) return;
    ms_state[r][c] = MS_REVEALED;
    ms_revealed++;
    if (ms_board[r][c] == 0) {
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++)
                ms_reveal(r + dr, c + dc);
    }
}

static void ms_check_win(void)
{
    if (ms_revealed == MS_ROWS * MS_COLS - MS_MINES)
        ms_game_over = 2;
}

static void ms_handle_click(int r, int c, int is_right)
{
    if (ms_game_over) return;
    if (r < 0 || r >= MS_ROWS || c < 0 || c >= MS_COLS) return;

    if (is_right) {
        if (ms_state[r][c] == MS_HIDDEN) {
            ms_state[r][c] = MS_FLAGGED;
            ms_flags++;
        } else if (ms_state[r][c] == MS_FLAGGED) {
            ms_state[r][c] = MS_HIDDEN;
            ms_flags--;
        }
        return;
    }

    if (ms_state[r][c] != MS_HIDDEN) return;

    if (ms_first_click) {
        ms_first_click = 0;
        ms_place_mines(r, c);
        ms_started = 1;
    }

    if (ms_board[r][c] == -1) {
        ms_state[r][c] = MS_REVEALED;
        ms_game_over = 1;
        for (int rr = 0; rr < MS_ROWS; rr++)
            for (int cc = 0; cc < MS_COLS; cc++)
                if (ms_board[rr][cc] == -1)
                    ms_state[rr][cc] = MS_REVEALED;
        return;
    }

    ms_reveal(r, c);
    ms_check_win();
}

static const fb_color_t ms_num_colors[9] = {
    0x00000000,   /* 0 */
    0x004A6CF7,   /* 1 - blue accent */
    0x002ECC71,   /* 2 - green */
    0x00E74C3C,   /* 3 - red */
    0x009B59B6,   /* 4 - purple */
    0x00E74C3C,   /* 5 - dark red */
    0x002ECC71,   /* 6 - teal */
    0x00E8E8F0,   /* 7 - white */
    0x008888A0,   /* 8 - gray */
};

static void draw_minesweeper_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "Minesweeper",
                      ws == &wins[focus_win], ws->maximized);

    int grid_w = MS_COLS * MS_CELL_SIZE;
    int grid_h = MS_ROWS * MS_CELL_SIZE;
    int gx = ws->x + (ws->w - grid_w) / 2;
    int gy = ws->y + TITLE_BAR_HEIGHT + 32;

    /* Info bar */
    {
        int bar_y = ws->y + TITLE_BAR_HEIGHT + 6;
        fb_color_t info_bg = COLOR_SURFACE_DIM;
        fill_rounded_rect(ws->x + 10, bar_y, ws->w - 20, 22, 4, info_bg);

        char buf[32];
        if (ms_game_over == 1) {
            fb_draw_string(ws->x + 18, bar_y + 4, "Game Over! - Click to restart",
                           COLOR_DANGER, info_bg);
        } else if (ms_game_over == 2) {
            fb_draw_string(ws->x + 18, bar_y + 4, "You Win! - Click to restart",
                           COLOR_SUCCESS, info_bg);
        } else {
            int_to_str(MS_MINES - ms_flags, buf);
            fb_draw_string(ws->x + 18, bar_y + 4, "Mines: ", COLOR_TEXT_BRIGHT, info_bg);
            fb_draw_string(ws->x + 66, bar_y + 4, buf, COLOR_WARNING, info_bg);
            if (!ms_started) {
                fb_draw_string(ws->x + 112, bar_y + 4, "Click to start",
                               COLOR_TEXT_DIM, info_bg);
            }
        }
    }

    /* Draw grid */
    for (int r = 0; r < MS_ROWS; r++) {
        for (int c = 0; c < MS_COLS; c++) {
            int cx = gx + c * MS_CELL_SIZE;
            int cy = gy + r * MS_CELL_SIZE;
            int cs = MS_CELL_SIZE - 1;

            if (ms_state[r][c] == MS_HIDDEN) {
                /* Unrevealed cell - modern raised look */
                fill_rounded_rect(cx, cy, cs, cs, 3, COLOR_MS_HIDDEN);
                /* Highlight (top-left) */
                fb_fill_rect(cx + 2, cy + 1, cs - 4, 1, COLOR_MS_HIDDEN_HI);
                fb_fill_rect(cx + 1, cy + 2, 1, cs - 4, COLOR_MS_HIDDEN_HI);
                /* Shadow (bottom-right) */
                fb_fill_rect(cx + 2, cy + cs - 2, cs - 4, 1, COLOR_MS_HIDDEN_SH);
                fb_fill_rect(cx + cs - 2, cy + 2, 1, cs - 4, COLOR_MS_HIDDEN_SH);
            } else if (ms_state[r][c] == MS_FLAGGED) {
                /* Flagged cell */
                fill_rounded_rect(cx, cy, cs, cs, 3, COLOR_MS_HIDDEN);
                fb_fill_rect(cx + 2, cy + 1, cs - 4, 1, COLOR_MS_HIDDEN_HI);
                fb_fill_rect(cx + 1, cy + 2, 1, cs - 4, COLOR_MS_HIDDEN_HI);
                fb_fill_rect(cx + 2, cy + cs - 2, cs - 4, 1, COLOR_MS_HIDDEN_SH);
                fb_fill_rect(cx + cs - 2, cy + 2, 1, cs - 4, COLOR_MS_HIDDEN_SH);

                /* Flag icon - modern style */
                int fcx = cx + cs / 2;
                int fcy = cy + cs / 2;
                /* Pole */
                fb_fill_rect(fcx + 1, fcy - 7, 2, 14, COLOR_TEXT_DIM);
                /* Flag triangle */
                fb_fill_rect(fcx - 5, fcy - 7, 6, 3, COLOR_DANGER);
                fb_fill_rect(fcx - 4, fcy - 4, 5, 2, COLOR_DANGER);
                fb_fill_rect(fcx - 3, fcy - 2, 4, 1, COLOR_DANGER);
                /* Base */
                fb_fill_rect(fcx - 4, fcy + 5, 10, 2, COLOR_TEXT_DIM);
            } else {
                /* Revealed cell */
                fill_rounded_rect(cx, cy, cs, cs, 3, COLOR_MS_REVEALED);
                fb_draw_rect(cx, cy, cs, cs, COLOR_MS_BORDER);

                if (ms_board[r][c] == -1) {
                    /* Mine */
                    int mcx = cx + cs / 2;
                    int mcy = cy + cs / 2;
                    fb_fill_rect(mcx - 4, mcy - 4, 8, 8, COLOR_TEXT_DIM);
                    fb_fill_rect(mcx - 6, mcy - 1, 12, 2, COLOR_TEXT_DIM);
                    fb_fill_rect(mcx - 1, mcy - 6, 2, 12, COLOR_TEXT_DIM);
                    fb_fill_rect(mcx - 2, mcy - 3, 2, 2, COLOR_TEXT_BRIGHT);
                } else if (ms_board[r][c] > 0) {
                    char num_str[2] = { '0' + ms_board[r][c], '\0' };
                    fb_draw_string(cx + cs / 2 - 4,
                                   cy + cs / 2 - 8,
                                   num_str,
                                   ms_num_colors[ms_board[r][c]],
                                   COLOR_MS_REVEALED);
                }
            }
        }
    }
}

/* ================================================================
 *  DESKTOP ICONS - Modern flat design, 48x48, 2-column grid
 * ================================================================ */

static void icon_grid_pos(int idx, int *ix, int *iy)
{
    int col = idx % ICON_GRID_COLS;
    int row = idx / ICON_GRID_COLS;
    *ix = ICON_GRID_LEFT + col * ICON_CELL_W + (ICON_CELL_W - ICON_SIZE) / 2;
    *iy = ICON_GRID_TOP + row * ICON_CELL_H;
}

/* Draw individual icon shapes - modern flat design with bold colors */
static void draw_icon_terminal(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? 0x00282848 :
                    hover  ? 0x00383860 : 0x002E2E50;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 10, bg);

    /* Subtle glow under icon on hover */
    if (hover) {
        fill_rounded_rect(ix - 2, iy + ICON_SIZE - 4, ICON_SIZE + 4, 8, 4,
                          0x004A6CF7);
    }

    /* Terminal screen */
    int sx = ix + 8, sy = iy + 8, sw = ICON_SIZE - 16, sh = ICON_SIZE - 20;
    fb_fill_rect(sx, sy, sw, sh, COLOR_TERM_BG);
    fb_draw_rect(sx, sy, sw, sh, COLOR_ACCENT);
    /* Prompt text */
    fb_draw_string(sx + 3, sy + 2, ">_", COLOR_TERM_FG, COLOR_TERM_BG);
    /* Bottom bar */
    fb_fill_rect(ix + 6, iy + ICON_SIZE - 10, ICON_SIZE - 12, 4, 0x00404060);
}

static void draw_icon_folder(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? 0x00282848 :
                    hover  ? 0x00383860 : 0x002E2E50;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 10, bg);

    if (hover) {
        fill_rounded_rect(ix - 2, iy + ICON_SIZE - 4, ICON_SIZE + 4, 8, 4,
                          0x004A6CF7);
    }

    /* Folder - modern flat style */
    int fx = ix + 7, fy = iy + 12;
    /* Tab */
    fill_rounded_rect(fx, fy - 4, 16, 6, 2, COLOR_ACCENT);
    /* Body */
    fill_rounded_rect(fx, fy, ICON_SIZE - 14, 22, 3, COLOR_ACCENT);
    /* Inner line */
    fb_draw_line(fx + 3, fy + 7, fx + ICON_SIZE - 17, fy + 7, COLOR_ACCENT_START);
}

static void draw_icon_sysmon(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? 0x00282848 :
                    hover  ? 0x00383860 : 0x002E2E50;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 10, bg);

    if (hover) {
        fill_rounded_rect(ix - 2, iy + ICON_SIZE - 4, ICON_SIZE + 4, 8, 4,
                          0x004A6CF7);
    }

    /* Bar chart - modern rounded bars */
    int bx = ix + 8, by = iy + 8;
    int bh[4] = {14, 24, 18, 28};
    fb_color_t bars[4] = {COLOR_ACCENT_START, COLOR_SUCCESS, COLOR_WARNING, COLOR_DANGER};
    for (int i = 0; i < 4; i++) {
        int bar_y = by + 30 - bh[i];
        fill_rounded_rect(bx + i * 8, bar_y, 6, bh[i], 2, bars[i]);
    }
}

static void draw_icon_settings(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? 0x00282848 :
                    hover  ? 0x00383860 : 0x002E2E50;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 10, bg);

    if (hover) {
        fill_rounded_rect(ix - 2, iy + ICON_SIZE - 4, ICON_SIZE + 4, 8, 4,
                          0x004A6CF7);
    }

    /* Gear icon - modern simplified */
    int cx = ix + ICON_SIZE / 2, cy = iy + ICON_SIZE / 2;
    /* Center hub */
    fb_fill_rect(cx - 4, cy - 4, 8, 8, COLOR_TEXT_DIM);
    fb_fill_rect(cx - 2, cy - 2, 4, 4, bg);
    /* Teeth */
    fb_fill_rect(cx - 2, cy - 10, 4, 5, COLOR_TEXT_DIM);
    fb_fill_rect(cx - 2, cy + 5, 4, 5, COLOR_TEXT_DIM);
    fb_fill_rect(cx - 10, cy - 2, 5, 4, COLOR_TEXT_DIM);
    fb_fill_rect(cx + 5, cy - 2, 5, 4, COLOR_TEXT_DIM);
    /* Diagonal teeth */
    fb_fill_rect(cx - 7, cy - 7, 4, 4, COLOR_TEXT_DIM);
    fb_fill_rect(cx + 3, cy - 7, 4, 4, COLOR_TEXT_DIM);
    fb_fill_rect(cx - 7, cy + 3, 4, 4, COLOR_TEXT_DIM);
    fb_fill_rect(cx + 3, cy + 3, 4, 4, COLOR_TEXT_DIM);
}

static void draw_icon_about(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? 0x00282848 :
                    hover  ? 0x00383860 : 0x002E2E50;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 10, bg);

    if (hover) {
        fill_rounded_rect(ix - 2, iy + ICON_SIZE - 4, ICON_SIZE + 4, 8, 4,
                          0x004A6CF7);
    }

    /* Info circle */
    int cx = ix + ICON_SIZE / 2, cy = iy + ICON_SIZE / 2;
    for (int dy = -12; dy <= 12; dy++) {
        for (int dx = -12; dx <= 12; dx++) {
            if (dx * dx + dy * dy <= 144 && dx * dx + dy * dy > 64) {
                int px = cx + dx, py = cy + dy;
                if (px >= ix && px < ix + ICON_SIZE && py >= iy && py < iy + ICON_SIZE)
                    fb_put_pixel(px, py, COLOR_ACCENT);
            }
        }
    }
    /* "i" */
    fb_fill_rect(cx - 2, cy - 7, 4, 4, COLOR_TEXT_BRIGHT);
    fb_fill_rect(cx - 2, cy - 1, 4, 10, COLOR_TEXT_BRIGHT);
}

static void draw_icon_home(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? 0x00282848 :
                    hover  ? 0x00383860 : 0x002E2E50;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 10, bg);

    if (hover) {
        fill_rounded_rect(ix - 2, iy + ICON_SIZE - 4, ICON_SIZE + 4, 8, 4,
                          0x004A6CF7);
    }

    /* House - modern flat */
    int bx = ix + 8, by = iy + 12;
    /* Roof triangle */
    for (int i = 0; i < 10; i++) {
        int hw = i + 1;
        int rx = bx + 16 - i;
        fb_fill_rect(rx, by + i, hw * 2, 1, COLOR_WARNING);
    }
    /* Body */
    fill_rounded_rect(bx + 5, by + 9, 22, 16, 2, COLOR_ACCENT);
    /* Door */
    fb_fill_rect(bx + 12, by + 15, 8, 10, COLOR_SUCCESS);
}

static void draw_icon_trash(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? 0x00282848 :
                    hover  ? 0x00383860 : 0x002E2E50;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 10, bg);

    if (hover) {
        fill_rounded_rect(ix - 2, iy + ICON_SIZE - 4, ICON_SIZE + 4, 8, 4,
                          0x004A6CF7);
    }

    /* Trash can - modern flat */
    int bx = ix + 11, by = iy + 10;
    /* Lid */
    fill_rounded_rect(bx - 3, by, 26, 5, 2, COLOR_TEXT_DIM);
    fb_fill_rect(bx + 7, by - 5, 8, 5, COLOR_TEXT_DIM);
    /* Body */
    fill_rounded_rect(bx, by + 5, 20, 20, 3, 0x00404060);
    /* Lines */
    fb_draw_line(bx + 5, by + 8, bx + 5, by + 22, COLOR_TEXT_DIM);
    fb_draw_line(bx + 10, by + 8, bx + 10, by + 22, COLOR_TEXT_DIM);
    fb_draw_line(bx + 15, by + 8, bx + 15, by + 22, COLOR_TEXT_DIM);
}

static void draw_icon_minesweeper(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? 0x00282848 :
                    hover  ? 0x00383860 : 0x002E2E50;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 10, bg);

    if (hover) {
        fill_rounded_rect(ix - 2, iy + ICON_SIZE - 4, ICON_SIZE + 4, 8, 4,
                          0x004A6CF7);
    }

    /* Mine icon - modern */
    int cx = ix + ICON_SIZE / 2, cy = iy + ICON_SIZE / 2;
    fb_fill_rect(cx - 6, cy - 6, 12, 12, COLOR_TEXT_DIM);
    fb_fill_rect(cx - 9, cy - 1, 18, 2, COLOR_TEXT_DIM);
    fb_fill_rect(cx - 1, cy - 9, 2, 18, COLOR_TEXT_DIM);
    /* Diagonal spikes */
    for (int d = -7; d <= -5; d++) {
        fb_put_pixel(cx + d, cy + d, COLOR_TEXT_DIM);
        fb_put_pixel(cx - d - 1, cy + d, COLOR_TEXT_DIM);
    }
    for (int d = 5; d <= 7; d++) {
        fb_put_pixel(cx + d, cy + d, COLOR_TEXT_DIM);
        fb_put_pixel(cx - d - 1, cy + d, COLOR_TEXT_DIM);
    }
    /* Highlight */
    fb_fill_rect(cx - 3, cy - 4, 2, 2, COLOR_TEXT_BRIGHT);
}

typedef void (*icon_draw_fn)(int, int, int, int);

static const icon_draw_fn icon_drawers[NUM_APPS] = {
    draw_icon_terminal,
    draw_icon_folder,
    draw_icon_sysmon,
    draw_icon_settings,
    draw_icon_about,
    draw_icon_home,
    draw_icon_trash,
    draw_icon_minesweeper
};

static void draw_desktop_icon(int idx, int hover, int pressed)
{
    int ix, iy;
    icon_grid_pos(idx, &ix, &iy);

    icon_drawers[idx](ix, iy, hover, pressed);

    /* Label below icon, centered with drop shadow */
    const char *name = app_names[idx];
    int len = 0;
    while (name[len]) len++;
    int label_x = ix + (ICON_SIZE - len * 8) / 2;
    if (label_x < 2) label_x = 2;
    int label_y = iy + ICON_SIZE + 6;

    /* Drop shadow */
    fb_draw_string(label_x + 1, label_y + 1, name, 0x00060610, COLOR_BG_TL);
    /* Text */
    fb_draw_string(label_x, label_y, name, hover ? COLOR_TEXT_BRIGHT : COLOR_TEXT, COLOR_BG_TL);
}

static void draw_all_desktop_icons(int hover_icon)
{
    for (int i = 0; i < NUM_APPS; i++)
        draw_desktop_icon(i, i == hover_icon, i == icon_pressed);
}

/* ================================================================
 *  MOUSE CURSOR
 * ================================================================ */

static void draw_mouse_cursor(int cx, int cy)
{
    static const uint16_t cursor_shape[18] = {
        0x0001, 0x0003, 0x0007, 0x000F, 0x001F, 0x003F,
        0x007F, 0x00FF, 0x01FF, 0x03FF, 0x07FF, 0x0FFF,
        0x07F0, 0x02C0, 0x01C0, 0x0080, 0x0080, 0x0000,
    };
    for (int row = 0; row < 18; row++) {
        uint16_t bits = cursor_shape[row];
        for (int col = 0; col < 12; col++) {
            if (bits & (1 << (11 - col))) {
                int px = cx + col, py = cy + row;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = col + dx, ny = row + dy;
                        int in_shape = 0;
                        if (ny >= 0 && ny < 18 && nx >= 0 && nx < 12)
                            if (cursor_shape[ny] & (1 << (11 - nx))) in_shape = 1;
                        if (!in_shape) {
                            int ox = px + dx, oy = py + dy;
                            if (ox >= 0 && oy >= 0 && ox < (int)fb_get_width() && oy < (int)fb_get_height())
                                fb_put_pixel(ox, oy, 0x00000000);
                        }
                    }
                if (px >= 0 && py >= 0 && px < (int)fb_get_width() && py < (int)fb_get_height())
                    fb_put_pixel(px, py, COLOR_TEXT_BRIGHT);
            }
        }
    }
}

/* Forward declaration */
static void draw_fox_face(int x, int y, int size);

/* ================================================================
 *  DOCK TASKBAR - Centered, floating, macOS/Windows 11 style
 * ================================================================ */

/* Dock geometry helpers */
static int dock_y(void)
{
    return (int)fb_get_height() - TASKBAR_HEIGHT - DOCK_FLOAT_GAP;
}

static int dock_total_width(void)
{
    /* Start button + window buttons + system tray */
    int start_w = 40;
    int win_w = num_wins * 44;
    int tray_w = 80;
    int spacing = 12;
    return start_w + spacing + win_w + spacing + tray_w + 16;
}

static int dock_x(void)
{
    int total_w = dock_total_width();
    int sw = (int)fb_get_width();
    int dx = (sw - total_w) / 2;
    if (dx < 10) dx = 10;
    return dx;
}

/* Position of start button within dock */
static void dock_start_rect(int *sx, int *sy, int *sw, int *sh)
{
    int dx = dock_x();
    int dy = dock_y();
    *sx = dx + 8;
    *sy = dy + 8;
    *sw = 36;
    *sh = TASKBAR_HEIGHT - 16;
}

/* Position of a window button within dock */
static void dock_win_rect(int slot, int *bx, int *by, int *bw, int *bh)
{
    int dx = dock_x();
    int dy = dock_y();
    int start_w = 40;
    int spacing = 12;
    *bx = dx + 8 + start_w + spacing + slot * 44;
    *by = dy + 8;
    *bw = 40;
    *bh = TASKBAR_HEIGHT - 16;
}

/* Position of system tray within dock */
static void dock_tray_rect(int *tx, int *ty, int *tw, int *th)
{
    int dx = dock_x();
    int dy = dock_y();
    int start_w = 40;
    int win_w = num_wins * 44;
    int spacing = 12;
    *tx = dx + 8 + start_w + spacing + win_w + spacing;
    *ty = dy + 8;
    *tw = 80;
    *th = TASKBAR_HEIGHT - 16;
}

static void draw_taskbar(void)
{
    uint32_t sw = fb_get_width();
    int dy = dock_y();
    int dx = dock_x();
    int total_w = dock_total_width();

    /* Dock background - frosted dark glass with rounded corners */
    fill_rounded_rect(dx, dy, total_w, TASKBAR_HEIGHT, DOCK_CORNER_R, COLOR_DOCK_BG);

    /* Subtle top border glow */
    {
        int line_x1 = dx + DOCK_CORNER_R;
        int line_x2 = dx + total_w - DOCK_CORNER_R;
        fb_draw_line(line_x1, dy, line_x2, dy, COLOR_DOCK_BORDER);
        /* Second glow line - slightly more visible */
        fb_draw_line(line_x1, dy + 1, line_x2, dy + 1, 0x00202040);
    }

    /* Border around dock */
    draw_rounded_rect(dx, dy, total_w, TASKBAR_HEIGHT, DOCK_CORNER_R, COLOR_DOCK_BORDER);

    /* === LEFT: Start button (rounded circle with fox logo) === */
    {
        int sx, sy, sw2, sh2;
        dock_start_rect(&sx, &sy, &sw2, &sh2);
        fb_color_t btn_bg = start_menu_open ? COLOR_ACCENT : COLOR_SURFACE;
        fill_rounded_rect(sx, sy, sw2, sh2, sw2 / 2, btn_bg);

        /* Glow on hover */
        if (start_menu_open) {
            fill_rounded_rect(sx - 2, sy - 2, sw2 + 4, sh2 + 4, sw2 / 2 + 2, 0x003838C0);
        }

        /* Fox face icon */
        draw_fox_face(sx + (sw2 - 20) / 2, sy + (sh2 - 20) / 2, 20);
    }

    /* === CENTER: Running apps as icon buttons === */
    for (int i = 0; i < num_wins; i++) {
        int bx, by, bw, bh;
        dock_win_rect(i, &bx, &by, &bw, &bh);
        int is_focused = (i == focus_win) && !wins[i].minimized;
        int is_minimized = wins[i].minimized;

        fb_color_t bg;
        if (is_focused) bg = COLOR_SURFACE_LIGHT;
        else if (is_minimized) bg = COLOR_SURFACE_DIM;
        else bg = COLOR_SURFACE;

        fill_rounded_rect(bx, by, bw, bh, 6, bg);

        /* Mini icon - simplified colored square */
        fb_color_t icon_c;
        switch (wins[i].app) {
        case 0: icon_c = COLOR_TERM_FG; break;
        case 1: icon_c = COLOR_ACCENT; break;
        case 2: icon_c = COLOR_SUCCESS; break;
        case 3: icon_c = COLOR_TEXT_DIM; break;
        case 4: icon_c = COLOR_ACCENT_START; break;
        case 5: icon_c = COLOR_WARNING; break;
        case 6: icon_c = COLOR_TEXT_DIM; break;
        case 7: icon_c = COLOR_DANGER; break;
        default: icon_c = COLOR_TEXT_DIM; break;
        }
        fill_rounded_rect(bx + 8, by + 6, 24, 24, 4, icon_c);
        /* Letter on icon */
        char letter[2] = { app_names[wins[i].app][0], '\0' };
        fb_draw_string(bx + 14, by + 10, letter, COLOR_TEXT_BRIGHT, icon_c);

        /* Active indicator dot below */
        if (is_focused) {
            fb_fill_rect(bx + bw / 2 - 3, by + bh - 4, 6, 3, COLOR_ACCENT);
        } else if (!is_minimized) {
            fb_fill_rect(bx + bw / 2 - 2, by + bh - 4, 4, 2, COLOR_DOCK_BORDER);
        }
    }

    /* === RIGHT: System tray === */
    {
        int tx, ty, tw2, th2;
        dock_tray_rect(&tx, &ty, &tw2, &th2);

        /* Time */
        uint64_t ms = timer_get_ms();
        uint64_t sec = ms / 1000;
        uint64_t min = sec / 60;
        sec = sec % 60;
        uint64_t hr = (min / 60) % 24;
        min = min % 60;
        char time_buf[12];
        int pos = 0;
        if (hr < 10) time_buf[pos++] = '0';
        { char tmp[4]; int t = 0; int n = (int)hr;
            if (n == 0) tmp[t++] = '0';
            else while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            for (int j = t - 1; j >= 0; j--) time_buf[pos++] = tmp[j]; }
        time_buf[pos++] = ':';
        if (min < 10) time_buf[pos++] = '0';
        { char tmp[4]; int t = 0; int n = (int)min;
            if (n == 0) tmp[t++] = '0';
            else while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            for (int j = t - 1; j >= 0; j--) time_buf[pos++] = tmp[j]; }
        time_buf[pos] = '\0';
        fb_draw_string(tx + 8, ty + 8, time_buf, COLOR_TEXT, COLOR_DOCK_BG);

        /* Status dots */
        int dot_x = tx + tw2 - 20;
        fb_fill_rect(dot_x, ty + 10, 6, 6, COLOR_SUCCESS);
        fb_fill_rect(dot_x + 10, ty + 10, 6, 6, COLOR_WARNING);
    }
}

/* ================================================================
 *  START MENU - Modern centered panel (Windows 11 / macOS style)
 * ================================================================ */

#define START_MENU_W          340
#define START_MENU_ITEM_H     32
#define START_MENU_HEADER_H   50
#define START_MENU_SEARCH_H   36
#define START_MENU_SECTION_H  24
#define START_MENU_SEP_H      8
#define START_MENU_POWER_H    36
#define START_MENU_USER_H     40
#define START_MENU_BOTTOM_PAD 8
#define START_MENU_GRID_COLS  4
#define START_MENU_GRID_ITEM  68

/* Menu item indices:
 * 0-7: Applications
 * 8:   Shutdown
 * 9:   Restart
 */
#define SM_ITEM_SHUTDOWN  8
#define SM_ITEM_RESTART   9

static int start_menu_height(void)
{
    return START_MENU_HEADER_H
         + START_MENU_SEARCH_H + 6
         + START_MENU_SECTION_H
         + ((NUM_APPS + START_MENU_GRID_COLS - 1) / START_MENU_GRID_COLS) * START_MENU_GRID_ITEM
         + START_MENU_SEP_H
         + START_MENU_POWER_H
         + START_MENU_USER_H
         + START_MENU_BOTTOM_PAD;
}

static int start_menu_x(void)
{
    return ((int)fb_get_width() - START_MENU_W) / 2;
}

static int start_menu_top(void)
{
    int mh = start_menu_height();
    int sh = (int)fb_get_height();
    int tb = dock_y();
    /* Position centered, but not overlapping dock */
    int top = (sh - mh - TASKBAR_HEIGHT - DOCK_FLOAT_GAP) / 2;
    if (top < 10) top = 10;
    return top;
}

/* Grid position for app in start menu */
static void sm_grid_pos(int idx, int *gx, int *gy)
{
    int mx = start_menu_x();
    int my = start_menu_top();
    int col = idx % START_MENU_GRID_COLS;
    int row = idx / START_MENU_GRID_COLS;
    int grid_start_x = mx + (START_MENU_W - START_MENU_GRID_COLS * START_MENU_GRID_ITEM) / 2;
    int grid_start_y = my + START_MENU_HEADER_H + START_MENU_SEARCH_H + 6 + START_MENU_SECTION_H;
    *gx = grid_start_x + col * START_MENU_GRID_ITEM;
    *gy = grid_start_y + row * START_MENU_GRID_ITEM;
}

static void draw_start_menu(void)
{
    int mx = start_menu_x();
    int my = start_menu_top();
    int mh = start_menu_height();

    /* Multi-layer shadow */
    fill_rounded_rect(mx + 6, my + 6, START_MENU_W, mh, 14, COLOR_SHADOW3);
    fill_rounded_rect(mx + 4, my + 4, START_MENU_W, mh, 13, COLOR_SHADOW2);
    fill_rounded_rect(mx + 2, my + 2, START_MENU_W, mh, 12, COLOR_SHADOW1);

    /* Background - frosted glass */
    fill_rounded_rect(mx, my, START_MENU_W, mh, 12, COLOR_GLASS);
    draw_rounded_rect(mx, my, START_MENU_W, mh, 12, COLOR_DOCK_BORDER);

    /* ---- Header ---- */
    {
        fill_rounded_rect_top(mx + 1, my + 1, START_MENU_W - 2, START_MENU_HEADER_H, 11, COLOR_SURFACE);
        /* Gradient accent line at bottom of header */
        fb_draw_line(mx + 20, my + START_MENU_HEADER_H - 1,
                     mx + START_MENU_W - 20, my + START_MENU_HEADER_H - 1, COLOR_ACCENT);

        draw_fox_face(mx + 14, my + 10, 28);
        fb_draw_string(mx + 50, my + 10, "SpiritFoxOS", COLOR_TEXT_BRIGHT, COLOR_SURFACE);
        fb_draw_string(mx + 50, my + 26, "v0.5.0", COLOR_TEXT_DIM, COLOR_SURFACE);
    }

    /* ---- Search bar ---- */
    {
        int sb_x = mx + 16;
        int sb_y = my + START_MENU_HEADER_H + 6;
        int sb_w = START_MENU_W - 32;
        fill_rounded_rect(sb_x, sb_y, sb_w, START_MENU_SEARCH_H, 6, COLOR_SURFACE_DIM);
        draw_rounded_rect(sb_x, sb_y, sb_w, START_MENU_SEARCH_H, 6, COLOR_DOCK_BORDER);
        fb_draw_string(sb_x + 10, sb_y + 10, "Search...", COLOR_TEXT_DIM, COLOR_SURFACE_DIM);
    }

    /* ---- Pinned section label ---- */
    {
        int sec_y = my + START_MENU_HEADER_H + START_MENU_SEARCH_H + 6;
        fb_draw_string(mx + 20, sec_y + 4, "PINNED", COLOR_TEXT_DIM, COLOR_GLASS);
    }

    /* ---- Pinned apps grid ---- */
    for (int i = 0; i < NUM_APPS; i++) {
        int gx, gy;
        sm_grid_pos(i, &gx, &gy);
        int hovered = (start_menu_hover == i);

        if (hovered)
            fill_rounded_rect(gx + 2, gy + 2, START_MENU_GRID_ITEM - 4, START_MENU_GRID_ITEM - 4,
                              6, COLOR_SURFACE_LIGHT);

        /* Draw a mini icon for each app */
        fb_color_t icon_c;
        switch (i) {
        case 0: icon_c = COLOR_TERM_FG; break;
        case 1: icon_c = COLOR_ACCENT; break;
        case 2: icon_c = COLOR_SUCCESS; break;
        case 3: icon_c = COLOR_TEXT_DIM; break;
        case 4: icon_c = COLOR_ACCENT_START; break;
        case 5: icon_c = COLOR_WARNING; break;
        case 6: icon_c = 0x00606080; break;
        case 7: icon_c = COLOR_DANGER; break;
        default: icon_c = COLOR_TEXT_DIM; break;
        }

        int icx = gx + (START_MENU_GRID_ITEM - 28) / 2;
        int icy = gy + 6;
        fill_rounded_rect(icx, icy, 28, 28, 6, icon_c);

        /* Letter */
        char letter[2] = { app_names[i][0], '\0' };
        fb_color_t letter_c = (i == 0) ? COLOR_TERM_BG : COLOR_TEXT_BRIGHT;
        fb_draw_string(icx + 9, icy + 6, letter, letter_c, icon_c);

        /* Label below icon */
        int label_y = icy + 32;
        fb_color_t txt = hovered ? COLOR_TEXT_BRIGHT : COLOR_TEXT;
        fb_color_t bg = hovered ? COLOR_SURFACE_LIGHT : COLOR_GLASS;
        fb_draw_string(gx + 4, label_y, app_names[i], txt, bg);
    }

    /* ---- Separator ---- */
    {
        int sep_y = my + START_MENU_HEADER_H + START_MENU_SEARCH_H + 6
                  + START_MENU_SECTION_H
                  + ((NUM_APPS + START_MENU_GRID_COLS - 1) / START_MENU_GRID_COLS) * START_MENU_GRID_ITEM
                  + START_MENU_SEP_H / 2;
        fb_draw_line(mx + 20, sep_y, mx + START_MENU_W - 20, sep_y, COLOR_DOCK_BORDER);
    }

    /* ---- Power row ---- */
    {
        int pw_y = my + START_MENU_HEADER_H + START_MENU_SEARCH_H + 6
                 + START_MENU_SECTION_H
                 + ((NUM_APPS + START_MENU_GRID_COLS - 1) / START_MENU_GRID_COLS) * START_MENU_GRID_ITEM
                 + START_MENU_SEP_H;
        int half_w = (START_MENU_W - 40) / 2;

        /* Shutdown button */
        {
            int hovered = (start_menu_hover == SM_ITEM_SHUTDOWN);
            fb_color_t bg = hovered ? COLOR_DANGER : COLOR_SURFACE_DIM;
            fill_rounded_rect(mx + 12, pw_y + 2, half_w, START_MENU_POWER_H - 4, 6, bg);
            /* Power icon - circle with gap */
            int icx = mx + 12 + 18, icy = pw_y + START_MENU_POWER_H / 2;
            for (int dy = -7; dy <= 7; dy++)
                for (int dx = -7; dx <= 7; dx++)
                    if (dx * dx + dy * dy <= 49 && dx * dx + dy * dy > 36)
                        fb_put_pixel(icx + dx, icy + dy, hovered ? COLOR_TEXT_BRIGHT : COLOR_DANGER);
            fb_fill_rect(icx - 1, icy - 9, 2, 5, hovered ? COLOR_TEXT_BRIGHT : COLOR_DANGER);
            fb_draw_string(mx + 12 + 32, pw_y + 10, "Shutdown",
                           hovered ? COLOR_TEXT_BRIGHT : COLOR_DANGER, bg);
        }

        /* Restart button */
        {
            int rx = mx + 12 + half_w + 16;
            int hovered = (start_menu_hover == SM_ITEM_RESTART);
            fb_color_t bg = hovered ? COLOR_WARNING : COLOR_SURFACE_DIM;
            fill_rounded_rect(rx, pw_y + 2, half_w, START_MENU_POWER_H - 4, 6, bg);
            int icx = rx + 18, icy = pw_y + START_MENU_POWER_H / 2;
            for (int dy = -7; dy <= 7; dy++)
                for (int dx = -7; dx <= 7; dx++)
                    if (dx * dx + dy * dy <= 49 && dx * dx + dy * dy > 36)
                        fb_put_pixel(icx + dx, icy + dy, hovered ? COLOR_TEXT_BRIGHT : COLOR_WARNING);
            fb_fill_rect(icx + 3, icy - 7, 4, 3, hovered ? COLOR_TEXT_BRIGHT : COLOR_WARNING);
            fb_draw_string(rx + 32, pw_y + 10, "Restart",
                           hovered ? COLOR_TEXT_BRIGHT : COLOR_WARNING, bg);
        }
    }

    /* ---- User bar ---- */
    {
        int uy = my + mh - START_MENU_USER_H - START_MENU_BOTTOM_PAD;
        fb_fill_rect(mx + 1, uy, START_MENU_W - 2, START_MENU_USER_H, COLOR_SURFACE_DIM);
        /* Rounded bottom corners */
        for (int i = 0; i < 12; i++) {
            int dx2 = 12 - i;
            fb_fill_rect(mx + 1, uy + START_MENU_USER_H - 1 - i, dx2, 1, COLOR_GLASS);
            fb_fill_rect(mx + START_MENU_W - 1 - dx2, uy + START_MENU_USER_H - 1 - i, dx2, 1, COLOR_GLASS);
        }

        /* Avatar circle */
        int ax = mx + 28, ay = uy + START_MENU_USER_H / 2;
        for (int dy2 = -12; dy2 <= 12; dy2++)
            for (int dx2 = -12; dx2 <= 12; dx2++)
                if (dx2 * dx2 + dy2 * dy2 <= 144)
                    fb_put_pixel(ax + dx2, ay + dy2, COLOR_ACCENT);
        /* Face in avatar */
        fb_fill_rect(ax - 4, ay - 4, 3, 3, COLOR_TEXT_BRIGHT);
        fb_fill_rect(ax + 1, ay - 4, 3, 3, COLOR_TEXT_BRIGHT);
        fb_fill_rect(ax - 3, ay + 2, 6, 2, COLOR_TERM_FG);

        fb_draw_string(mx + 48, uy + 10, "user", COLOR_TEXT, COLOR_SURFACE_DIM);
        fb_draw_string(mx + 48, uy + 24, "Administrator", COLOR_TEXT_DIM, COLOR_SURFACE_DIM);
    }
}

static int point_in_start_menu(int px, int py)
{
    if (!start_menu_open) return 0;
    int mx = start_menu_x();
    int my = start_menu_top();
    int mh = start_menu_height();
    return (px >= mx && px < mx + START_MENU_W && py >= my && py < my + mh);
}

static int start_menu_item_at(int px, int py)
{
    if (!start_menu_open) return -1;

    /* Pinned apps grid */
    for (int i = 0; i < NUM_APPS; i++) {
        int gx, gy;
        sm_grid_pos(i, &gx, &gy);
        if (px >= gx && px < gx + START_MENU_GRID_ITEM &&
            py >= gy && py < gy + START_MENU_GRID_ITEM)
            return i;
    }

    /* Power row */
    int mh = start_menu_height();
    int mx = start_menu_x();
    int my = start_menu_top();
    int pw_y = my + START_MENU_HEADER_H + START_MENU_SEARCH_H + 6
             + START_MENU_SECTION_H
             + ((NUM_APPS + START_MENU_GRID_COLS - 1) / START_MENU_GRID_COLS) * START_MENU_GRID_ITEM
             + START_MENU_SEP_H;
    int half_w = (START_MENU_W - 40) / 2;
    if (py >= pw_y && py < pw_y + START_MENU_POWER_H) {
        if (px >= mx + 12 && px < mx + 12 + half_w)
            return SM_ITEM_SHUTDOWN;
        if (px >= mx + 12 + half_w + 16 && px < mx + START_MENU_W - 12)
            return SM_ITEM_RESTART;
    }

    return -1;
}

/* ================================================================
 *  DESKTOP CONTEXT MENU (Right-click) - Modern frosted glass
 * ================================================================ */

#define CTX_MENU_W  200
#define CTX_MENU_ITEM_H 32

static const char *ctx_menu_items[] = {
    "Open Terminal",
    "Open File Manager",
    "System Monitor",
    "Settings",
    "---",
    "About SpiritFoxOS",
    "Shutdown"
};
#define CTX_MENU_COUNT 7

static void draw_ctx_menu(void)
{
    if (!ctx_menu_open) return;
    int mx = ctx_menu_x, my = ctx_menu_y;
    int menu_h = CTX_MENU_COUNT * CTX_MENU_ITEM_H + 12;

    if (mx + CTX_MENU_W > (int)fb_get_width()) mx = (int)fb_get_width() - CTX_MENU_W;
    if (my + menu_h > dock_y()) my = dock_y() - menu_h;

    /* Shadow */
    fill_rounded_rect(mx + 4, my + 4, CTX_MENU_W, menu_h, 10, COLOR_SHADOW2);

    /* Background - frosted glass */
    fill_rounded_rect(mx, my, CTX_MENU_W, menu_h, 10, COLOR_GLASS);
    draw_rounded_rect(mx, my, CTX_MENU_W, menu_h, 10, COLOR_DOCK_BORDER);

    int iy = my + 6;
    for (int i = 0; i < CTX_MENU_COUNT; i++) {
        if (strcmp(ctx_menu_items[i], "---") == 0) {
            /* Separator with opacity */
            fb_draw_line(mx + 16, iy + CTX_MENU_ITEM_H / 2,
                         mx + CTX_MENU_W - 16, iy + CTX_MENU_ITEM_H / 2,
                         COLOR_DOCK_BORDER);
            iy += CTX_MENU_ITEM_H;
            continue;
        }
        int hovered = (ctx_menu_hover == i);
        if (hovered)
            fill_rounded_rect(mx + 4, iy, CTX_MENU_W - 8, CTX_MENU_ITEM_H, 4, COLOR_SURFACE_LIGHT);

        fb_color_t txt = hovered ? COLOR_TEXT_BRIGHT : COLOR_TEXT;
        fb_color_t bg = hovered ? COLOR_SURFACE_LIGHT : COLOR_GLASS;
        if (i == CTX_MENU_COUNT - 1 && !hovered) txt = COLOR_DANGER;
        fb_draw_string(mx + 16, iy + 8, ctx_menu_items[i], txt, bg);
        iy += CTX_MENU_ITEM_H;
    }
}

static int point_in_ctx_menu(int px, int py)
{
    if (!ctx_menu_open) return 0;
    int menu_h = CTX_MENU_COUNT * CTX_MENU_ITEM_H + 12;
    int mx = ctx_menu_x, my = ctx_menu_y;
    if (mx + CTX_MENU_W > (int)fb_get_width()) mx = (int)fb_get_width() - CTX_MENU_W;
    if (my + menu_h > dock_y()) my = dock_y() - menu_h;
    return (px >= mx && px < mx + CTX_MENU_W && py >= my && py < my + menu_h);
}

static int ctx_menu_item_at(int px, int py)
{
    if (!ctx_menu_open) return -1;
    int mx = ctx_menu_x, my = ctx_menu_y;
    int menu_h = CTX_MENU_COUNT * CTX_MENU_ITEM_H + 12;
    if (mx + CTX_MENU_W > (int)fb_get_width()) mx = (int)fb_get_width() - CTX_MENU_W;
    if (my + menu_h > dock_y()) my = dock_y() - menu_h;

    int iy = my + 6;
    for (int i = 0; i < CTX_MENU_COUNT; i++) {
        if (px >= mx && px < mx + CTX_MENU_W && py >= iy && py < iy + CTX_MENU_ITEM_H)
            return i;
        iy += CTX_MENU_ITEM_H;
    }
    return -1;
}

/* ================================================================
 *  WINDOW FRAME - Modern minimalist with macOS-style dots
 * ================================================================ */

/* Control button positions for a window */
static void win_control_positions(int wx, int wy, int ww __attribute__((unused)),
                                  int *close_x, int *min_x, int *max_x, int *btn_y)
{
    *btn_y = wy + 8;
    *close_x = wx + 12;
    *min_x = wx + 38;
    *max_x = wx + 64;
}

static void draw_window_frame(int x, int y, int w, int h, const char *title,
                              int active, int maximized)
{
    /* Multi-layer shadow */
    if (!maximized) {
        draw_window_shadow(x, y, w, h);
    }

    /* Window body */
    fill_rounded_rect_top(x, y + TITLE_BAR_HEIGHT, w, h - TITLE_BAR_HEIGHT, 0, COLOR_WIN_BODY);

    /* Title bar - semi-transparent dark */
    fill_rounded_rect_top(x, y, w, TITLE_BAR_HEIGHT, WIN_CORNER_R, COLOR_WIN_TITLE);

    /* Clear top corners to create rounded shape */
    clear_top_corners_rounded(x, y, w, TITLE_BAR_HEIGHT, WIN_CORNER_R, COLOR_BG_TL);

    /* Redraw rounded top corners with proper color */
    for (int i = 0; i < WIN_CORNER_R; i++) {
        int dx = WIN_CORNER_R - i;
        fb_fill_rect(x + dx, y + i, 1, 1, COLOR_WIN_TITLE);
        fb_fill_rect(x + w - 1 - dx, y + i, 1, 1, COLOR_WIN_TITLE);
    }

    /* Title text - centered */
    {
        int len = 0;
        while (title[len]) len++;
        int title_x = x + (w - len * 8) / 2;
        fb_draw_string(title_x, y + 10, title, active ? COLOR_TEXT_BRIGHT : COLOR_TEXT_DIM, COLOR_WIN_TITLE);
    }

    /* Control buttons - macOS style colored dots */
    int close_x, min_x, max_x, btn_y;
    win_control_positions(x, y, w, &close_x, &min_x, &max_x, &btn_y);

    /* Close - red dot (radius 7) */
    {
        int cx2 = close_x + 7, cy2 = btn_y + 7;
        for (int dy2 = -7; dy2 <= 7; dy2++)
            for (int dx2 = -7; dx2 <= 7; dx2++)
                if (dx2 * dx2 + dy2 * dy2 <= 49)
                    fb_put_pixel(cx2 + dx2, cy2 + dy2, COLOR_DANGER);
    }

    /* Minimize - yellow dot (radius 7) */
    {
        int cx2 = min_x + 7, cy2 = btn_y + 7;
        for (int dy2 = -7; dy2 <= 7; dy2++)
            for (int dx2 = -7; dx2 <= 7; dx2++)
                if (dx2 * dx2 + dy2 * dy2 <= 49)
                    fb_put_pixel(cx2 + dx2, cy2 + dy2, COLOR_WARNING);
    }

    /* Maximize - green dot (radius 7) */
    {
        int cx2 = max_x + 7, cy2 = btn_y + 7;
        for (int dy2 = -7; dy2 <= 7; dy2++)
            for (int dx2 = -7; dx2 <= 7; dx2++)
                if (dx2 * dx2 + dy2 * dy2 <= 49)
                    fb_put_pixel(cx2 + dx2, cy2 + dy2, COLOR_SUCCESS);
    }

    /* Subtle border - only if active */
    if (active) {
        draw_rounded_rect(x, y, w, h, WIN_CORNER_R, COLOR_DOCK_BORDER);
    }

    /* Resize handle - subtle, only at bottom-right */
    if (!maximized) {
        int rx = x + w - 4, ry = y + h - 4;
        fb_put_pixel(rx, ry, COLOR_DOCK_BORDER);
        fb_put_pixel(rx - 3, ry, COLOR_DOCK_BORDER);
        fb_put_pixel(rx, ry - 3, COLOR_DOCK_BORDER);
        fb_put_pixel(rx - 6, ry, COLOR_DOCK_BORDER);
        fb_put_pixel(rx, ry - 6, COLOR_DOCK_BORDER);
        fb_put_pixel(rx - 3, ry - 3, COLOR_DOCK_BORDER);
    }
}

/* ================================================================
 *  APPLICATION WINDOW DRAWING
 * ================================================================ */

static void draw_terminal_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "Terminal",
                      ws == &wins[focus_win], ws->maximized);

    /* Terminal background - dark */
    int tx = ws->x + 2;
    int ty_body = ws->y + TITLE_BAR_HEIGHT;
    int tw = ws->w - 4;
    int th = ws->h - TITLE_BAR_HEIGHT - 2;
    fb_fill_rect(tx, ty_body, tw, th, COLOR_TERM_BG);

    int tx2 = ws->x + 12, ty2 = ty_body + 8;
    int term_rows = (th - 40) / 16;
    if (term_rows > TERM_BUF_LINES) term_rows = TERM_BUF_LINES;
    int start_line = term_cursor_line - term_rows + 1;
    if (start_line < 0) start_line = 0;
    for (int i = 0; i < term_rows; i++) {
        int li = start_line + i;
        if (li >= 0 && li < TERM_BUF_LINES)
            fb_draw_string(tx2, ty2 + i * 16, term_buf[li], COLOR_TERM_FG, COLOR_TERM_BG);
    }

    /* Modern prompt */
    int input_y = ty2 + term_rows * 16;
    fb_draw_string(tx2, input_y, "user@spiritfox:~$ ", COLOR_TERM_PROMPT, COLOR_TERM_BG);
    int prompt_w = 18 * 8;
    fb_draw_string(tx2 + prompt_w, input_y, term_input, COLOR_TEXT_BRIGHT, COLOR_TERM_BG);

    /* Blinking block cursor */
    if ((timer_get_ms() / 500) % 2 == 0) {
        int cursor_x = tx2 + prompt_w + term_input_len * 8;
        fb_fill_rect(cursor_x, input_y, 8, 16, COLOR_TERM_FG);
    }
}

static void draw_sysmon_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "System Monitor",
                      ws == &wins[focus_win], ws->maximized);

    int tx = ws->x + 16, ty = ws->y + TITLE_BAR_HEIGHT + 12;
    int content_w = ws->w - 32;

    /* Header */
    fb_draw_string(tx, ty, "System Monitor", COLOR_TEXT_BRIGHT, COLOR_WIN_BODY);
    ty += 28;

    /* Card: Display Info */
    {
        fill_rounded_rect(tx, ty, content_w, 52, 6, COLOR_SURFACE);
        fb_draw_string(tx + 12, ty + 6, "Display", COLOR_ACCENT, COLOR_SURFACE);
        char buf[32]; int pos = 0; uint32_t w2 = fb_get_width(), h2 = fb_get_height();
        int n = (int)w2; char tmp[12]; int t = 0;
        if (n == 0) buf[pos++] = '0';
        else { while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            for (int j = t - 1; j >= 0; j--) buf[pos++] = tmp[j]; }
        buf[pos++] = 'x'; n = (int)h2; t = 0;
        if (n == 0) buf[pos++] = '0';
        else { while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            for (int j = t - 1; j >= 0; j--) buf[pos++] = tmp[j]; }
        buf[pos++] = 'x'; buf[pos++] = '3'; buf[pos++] = '2'; buf[pos] = '\0';
        fb_draw_string(tx + 12, ty + 24, buf, COLOR_TEXT, COLOR_SURFACE);
        fb_draw_string(tx + 100, ty + 24, "32bpp", COLOR_TEXT_DIM, COLOR_SURFACE);
        ty += 60;
    }

    /* Card: Uptime */
    {
        fill_rounded_rect(tx, ty, content_w, 52, 6, COLOR_SURFACE);
        fb_draw_string(tx + 12, ty + 6, "Uptime", COLOR_ACCENT, COLOR_SURFACE);
        char buf[32]; int pos = 0; int n = (int)(timer_get_ms() / 1000);
        char tmp[12]; int t = 0;
        if (n == 0) buf[pos++] = '0';
        else { while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            for (int j = t - 1; j >= 0; j--) buf[pos++] = tmp[j]; }
        buf[pos++] = 's'; buf[pos] = '\0';
        fb_draw_string(tx + 12, ty + 24, buf, COLOR_TEXT, COLOR_SURFACE);
        ty += 60;
    }

    /* Card: CPU Usage */
    {
        fill_rounded_rect(tx, ty, content_w, 52, 6, COLOR_SURFACE);
        fb_draw_string(tx + 12, ty + 6, "CPU Usage", COLOR_ACCENT, COLOR_SURFACE);
        /* Progress bar */
        int bar_x = tx + 12, bar_y2 = ty + 26, bar_w = content_w - 24;
        fill_rounded_rect(bar_x, bar_y2, bar_w, 14, 4, COLOR_SURFACE_DIM);
        int usage = (int)((timer_get_ms() / 100) % 80);
        int fill_w = bar_w * usage / 100;
        if (fill_w > 4) {
            fb_color_t bar_c = usage > 60 ? COLOR_WARNING : COLOR_SUCCESS;
            fill_rounded_rect(bar_x, bar_y2, fill_w, 14, 4, bar_c);
        }
        ty += 60;
    }

    /* Card: Memory */
    {
        fill_rounded_rect(tx, ty, content_w, 52, 6, COLOR_SURFACE);
        fb_draw_string(tx + 12, ty + 6, "Memory", COLOR_ACCENT, COLOR_SURFACE);
        int bar_x = tx + 12, bar_y2 = ty + 26, bar_w = content_w - 24;
        fill_rounded_rect(bar_x, bar_y2, bar_w, 14, 4, COLOR_SURFACE_DIM);
        int mem_pct = 40;
        int fill_w = bar_w * mem_pct / 100;
        if (fill_w > 4) {
            fill_rounded_rect(bar_x, bar_y2, fill_w, 14, 4, COLOR_ACCENT);
        }
        ty += 60;
    }

    /* Card: Windows */
    {
        fill_rounded_rect(tx, ty, content_w, 40, 6, COLOR_SURFACE);
        fb_draw_string(tx + 12, ty + 6, "Windows: ", COLOR_TEXT_DIM, COLOR_SURFACE);
        char buf[8]; buf[0] = '0' + num_wins; buf[1] = '\0';
        fb_draw_string(tx + 80, ty + 6, buf, COLOR_TEXT, COLOR_SURFACE);
        fb_draw_string(tx + 12, ty + 22, "GUI: Modern WM", COLOR_TEXT_DIM, COLOR_SURFACE);
    }
}

static void draw_about_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "About SpiritFoxOS",
                      ws == &wins[focus_win], ws->maximized);

    int cx = ws->x + ws->w / 2;
    int ty = ws->y + TITLE_BAR_HEIGHT + 20;

    /* Large fox logo */
    draw_fox_face(cx - 30, ty, 60);
    ty += 72;

    /* Gradient text effect for title */
    fb_draw_string(cx - 11 * 8 / 2, ty, "SpiritFoxOS", COLOR_ACCENT, COLOR_WIN_BODY);
    ty += 22;
    fb_draw_string(cx - 8 * 8 / 2, ty, "v0.5.0", COLOR_TEXT_DIM, COLOR_WIN_BODY);
    ty += 24;

    /* Separator */
    fb_draw_line(ws->x + 30, ty, ws->x + ws->w - 30, ty, COLOR_DOCK_BORDER);
    ty += 16;

    fb_draw_string(cx - 18 * 8 / 2, ty, "Modern Window Manager", COLOR_TEXT, COLOR_WIN_BODY);
    ty += 20;
    fb_draw_string(cx - 20 * 8 / 2, ty, "Mouse-Supported GUI", COLOR_TERM_PROMPT, COLOR_WIN_BODY);
    ty += 20;
    fb_draw_string(cx - 19 * 8 / 2, ty, "Double-buffered FB", COLOR_TEXT_DIM, COLOR_WIN_BODY);
    ty += 28;
    fb_draw_string(cx - 25 * 8 / 2, ty, "Right-click desktop for menu", COLOR_ACCENT, COLOR_WIN_BODY);
}

static void draw_filemanager_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "File Manager",
                      ws == &wins[focus_win], ws->maximized);

    /* Sidebar */
    int sidebar_w = 120;
    fill_rounded_rect(ws->x + 2, ws->y + TITLE_BAR_HEIGHT, sidebar_w,
                      ws->h - TITLE_BAR_HEIGHT - 2, 0, COLOR_SURFACE);

    int sx = ws->x + 10, sy = ws->y + TITLE_BAR_HEIGHT + 10;
    fb_draw_string(sx, sy, "Places", COLOR_ACCENT, COLOR_SURFACE); sy += 22;
    fb_draw_string(sx, sy, "Home", COLOR_TEXT, COLOR_SURFACE); sy += 18;
    fb_draw_string(sx, sy, "Desktop", COLOR_TEXT, COLOR_SURFACE); sy += 18;
    fb_draw_string(sx, sy, "Documents", COLOR_TEXT, COLOR_SURFACE); sy += 18;
    fb_draw_string(sx, sy, "Downloads", COLOR_TEXT, COLOR_SURFACE); sy += 18;

    /* Content area */
    int tx = ws->x + sidebar_w + 12;
    int ty = ws->y + TITLE_BAR_HEIGHT + 10;
    fb_draw_string(tx, ty, "Path: /", COLOR_TEXT_BRIGHT, COLOR_WIN_BODY); ty += 24;

    const char *entries[] = {
        "  bin/", "  dev/", "  etc/", "  home/",
        "  mnt/", "  opt/", "  proc/", "  root/",
        "  sbin/", "  sys/", "  tmp/", "  usr/", "  var/"
    };
    for (int i = 0; i < 13; i++) {
        fb_draw_string(tx, ty, entries[i], COLOR_TERM_PROMPT, COLOR_WIN_BODY);
        ty += 18;
    }
}

static void draw_settings_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "Settings",
                      ws == &wins[focus_win], ws->maximized);

    int tx = ws->x + 20, ty = ws->y + TITLE_BAR_HEIGHT + 14;
    int content_w = ws->w - 40;

    /* Display section */
    fb_draw_string(tx, ty, "Display", COLOR_TEXT_BRIGHT, COLOR_WIN_BODY); ty += 24;
    /* Card */
    fill_rounded_rect(tx, ty, content_w, 56, 6, COLOR_SURFACE);
    fb_draw_string(tx + 12, ty + 8, "Resolution: VBE default", COLOR_TEXT, COLOR_SURFACE);
    fb_draw_string(tx + 12, ty + 28, "Color depth: 32-bit XRGB8888", COLOR_TEXT_DIM, COLOR_SURFACE);
    ty += 68;

    /* Keyboard section */
    fb_draw_string(tx, ty, "Keyboard", COLOR_TEXT_BRIGHT, COLOR_WIN_BODY); ty += 24;
    fill_rounded_rect(tx, ty, content_w, 36, 6, COLOR_SURFACE);
    fb_draw_string(tx + 12, ty + 8, "Layout: US (Scancode Set 1)", COLOR_TEXT, COLOR_SURFACE);
    ty += 48;

    /* Mouse section */
    fb_draw_string(tx, ty, "Mouse", COLOR_TEXT_BRIGHT, COLOR_WIN_BODY); ty += 24;
    fill_rounded_rect(tx, ty, content_w, 36, 6, COLOR_SURFACE);
    fb_draw_string(tx + 12, ty + 8, "PS/2 compatible, 3 buttons", COLOR_TEXT, COLOR_SURFACE);
    ty += 48;

    /* Shortcuts section */
    fb_draw_string(tx, ty, "Shortcuts", COLOR_TEXT_BRIGHT, COLOR_WIN_BODY); ty += 24;
    fill_rounded_rect(tx, ty, content_w, 52, 6, COLOR_SURFACE);
    fb_draw_string(tx + 12, ty + 8, "Alt+Tab: Switch window", COLOR_TEXT, COLOR_SURFACE);
    fb_draw_string(tx + 12, ty + 28, "Esc: Exit GUI", COLOR_TEXT_DIM, COLOR_SURFACE);
}

static void draw_home_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "Home",
                      ws == &wins[focus_win], ws->maximized);

    int tx = ws->x + 14, ty = ws->y + TITLE_BAR_HEIGHT + 10;
    fb_draw_string(tx, ty, "/home/user", COLOR_TEXT_BRIGHT, COLOR_WIN_BODY); ty += 24;

    /* Folder items as small cards */
    const char *items[] = { "Documents/", "Downloads/", "Pictures/", "Music/", "Desktop/" };
    for (int i = 0; i < 5; i++) {
        fill_rounded_rect(tx, ty, ws->w - 28, 24, 4, COLOR_SURFACE);
        /* Folder icon */
        fill_rounded_rect(tx + 6, ty + 5, 14, 10, 2, COLOR_ACCENT);
        fb_draw_string(tx + 26, ty + 4, items[i], COLOR_TERM_PROMPT, COLOR_SURFACE);
        ty += 30;
    }
}

static void draw_trash_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "Trash",
                      ws == &wins[focus_win], ws->maximized);

    int cx = ws->x + ws->w / 2;
    int ty = ws->y + TITLE_BAR_HEIGHT + 30;

    /* Empty state icon */
    fill_rounded_rect(cx - 16, ty, 32, 32, 6, COLOR_SURFACE_DIM);
    fb_draw_string(cx - 4, ty + 8, "X", COLOR_TEXT_DIM, COLOR_SURFACE_DIM);
    ty += 44;

    fb_draw_string(cx - 6 * 8 / 2, ty, "Trash is empty", COLOR_TEXT_DIM, COLOR_WIN_BODY);
}

static void draw_app_window(WinSlot *ws)
{
    switch (ws->app) {
    case 0: draw_terminal_window(ws); break;
    case 1: draw_filemanager_window(ws); break;
    case 2: draw_sysmon_window(ws); break;
    case 3: draw_settings_window(ws); break;
    case 4: draw_about_window(ws); break;
    case 5: draw_home_window(ws); break;
    case 6: draw_trash_window(ws); break;
    case 7: draw_minesweeper_window(ws); break;
    }
}

/* ================================================================
 *  FOX FACE LOGO
 * ================================================================ */

static const uint32_t fox_logo_20[20 * 20] = {
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0090412B,0x00A34E30,0x00A95736,0x00000000,0x00000000,0x00000000,0x00000000,0x00A55234,0x009D4B2F,0x0096462D,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A75735,0x00C56E41,0x00D37C48,0x00D9824B,0x00C06F41,0x00BD6B3F,0x00D47F49,0x00D07846,0x00B6633B,0x00AE5A37,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00C37042,0x00DB874C,0x00E08C4F,0x00D5824B,0x00CC7645,0x00C77142,0x00CC7A46,0x00DE8B4F,0x00D5824A,0x00C97243,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00D4814A,0x00E59153,0x00DB894F,0x00D5834C,0x00D6834A,0x00D7834B,0x00D2804A,0x00D2804B,0x00E08C51,0x00E28A4F,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00D68249,0x00EE924F,0x00E08846,0x00E48C4C,0x00E18E51,0x00E18E51,0x00E69151,0x00E18745,0x00D98345,0x00F19453,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00DB9060,0x00EFBF98,0x00CAC3AA,0x00E7A368,0x00E58D4B,0x00E48F4F,0x00E69455,0x00DBC9A8,0x00E1BD9D,0x00E89D6A,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00EADECF,0x00EE9F65,0x00F99E5A,0x00EACEB6,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009F4F2F,0x00A8512F,0x00AA5028,0x00B9653E,0x00000000,0x00D29C75,0x00000000,0x00FD9B4D,0x00000000,0x00000000,0x00D3D0D0,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A44E31,0x009A4B2F,0x00AA5B39,0x00D87D48,0x00C8733F,0x00000000,0x00E18A4B,0x00F59957,0x00000000,0x00E0E7EB,0x00E3E1E1,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0097452C,0x009E4D30,0x00AC5B37,0x00C97645,0x00EA9253,0x00000000,0x00000000,0x00000000,0x00000000,0x00FFFFFF,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A75333,0x00BB643C,0x00CA7645,0x00DF8B50,0x00EA9251,0x00E4A373,0x00F0D7C5,0x00FFF5E4,0x00EBCAB4,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00B46039,0x00BE6B3F,0x00CE7B47,0x00DC884E,0x00DF884B,0x00DE884C,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
};

static const uint32_t fox_logo_28[28 * 28] = {
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00924229,0x00AC5133,0x00A04F31,0x00AA5836,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A55334,0x009D4C30,0x00A75032,0x0099482D,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A15032,0x00B6613A,0x00BB693E,0x00CB7645,0x00D67E49,0x00D07B47,0x00C06F40,0x00BD6C3F,0x00D17C47,0x00D47C48,0x00C57042,0x00B4623B,0x00A95936,0x00AB5635,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00B4623A,0x00CA7645,0x00D17D48,0x00D9874C,0x00E18E50,0x00D7834B,0x00C66F42,0x00C0693F,0x00CD7946,0x00DF8C4F,0x00DA874C,0x00CB7846,0x00BE6C40,0x00BD663D,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00C47142,0x00D9854C,0x00DE8B4F,0x00E18D50,0x00D4814A,0x00C77544,0x00C97545,0x00C67444,0x00BF6E40,0x00C97645,0x00DD894E,0x00DE8B4F,0x00D27E49,0x00CF7746,0x00B25F3A,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00CE7C47,0x00E59051,0x00E18E50,0x00D9864D,0x00D8844C,0x00D9854C,0x00D7844B,0x00D7844B,0x00D8854C,0x00D3804A,0x00CE7C48,0x00DB884E,0x00E18E50,0x00E1884E,0x00BF6D3F,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00D38049,0x00E79253,0x00E18C4F,0x00E58E50,0x00E08E51,0x00DF8B4F,0x00DE8A4F,0x00DF8B4F,0x00E08D50,0x00E29052,0x00E18C50,0x00D9834A,0x00DF8C50,0x00EA9152,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00CF7D47,0x00F59551,0x00DC8D4F,0x00D78D50,0x00E78D4B,0x00E59153,0x00E49051,0x00E49051,0x00E49153,0x00E88F4F,0x00DF8E4E,0x00D48D50,0x00E48A49,0x00F49856,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00F7C099,0x00E1CDB9,0x00C8DACD,0x00D3B084,0x00E38845,0x00E49153,0x00E59254,0x00E68C49,0x00D2945D,0x00CCD7C2,0x00E0DAC7,0x00F4C5A4,0x00E5A576,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00ECFFFF,0x00F0B68A,0x00EC924F,0x00E58E4D,0x00F6A568,0x00E4E0D6,0x00FDFFFF,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A95431,0x00A5502C,0x00A84F27,0x00000000,0x00000000,0x00E9A878,0x00EF9C5E,0x00F8E3D3,0x00000000,0x00000000,0x00000000,0x00000000,0x00CECBC8,0x00CDCDCD,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A35032,0x00A35032,0x00A45333,0x00B3603B,0x00C76D40,0x00BA6030,0x00000000,0x00000000,0x00000000,0x00E38A46,0x00FBA15C,0x00E19055,0x00000000,0x00000000,0x00DFDEDD,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A34D31,0x0097482E,0x009E4E30,0x00AA5836,0x00BA693E,0x00DF854E,0x00CB7641,0x00000000,0x00E48F51,0x00FFAC63,0x00EF9757,0x00D27E48,0x00000000,0x00DEDCDC,0x00E8E6E6,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009D492F,0x0097482D,0x009F4F31,0x00AC5B37,0x00BE6C40,0x00D6834B,0x00E28C50,0x00000000,0x00000000,0x00DF8041,0x00000000,0x00000000,0x00EAE8E6,0x00FDFEFF,0x00F1EFF0,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0095452C,0x00A54F31,0x009F4F31,0x00AD5D38,0x00C06E41,0x00D27F49,0x00E48F52,0x00DF884D,0x00000000,0x00000000,0x00000000,0x00F2F6FA,0x00F6FCFF,0x00FFF6F1,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009E4C30,0x00B25A38,0x00B3603A,0x00BE6C40,0x00CE7C47,0x00DD8A4E,0x00E69151,0x00EA9355,0x00ECB186,0x00FEDFC9,0x00FFECDA,0x00F2CDB4,0x00E1AB85,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00AB5936,0x00B35F39,0x00BC683E,0x00D17A47,0x00E2894F,0x00EC9354,0x00EE9554,0x00EA8C49,0x00DC8242,0x00DD8343,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00C57143,0x00CF7A46,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
};

static const uint32_t fox_logo_48[48 * 48] = {
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0096422A,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009C472D,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x008E3F29,0x00A1492F,0x0098462D,0x0099482D,0x009E4C30,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009E4C30,0x0099482E,0x009A482E,0x009E4A2F,0x00A44C31,0x0094452B,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009E4A2F,0x009C4C2F,0x00A15132,0x00AA5836,0x00B45E39,0x00B15E39,0x00B25F39,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00B05D38,0x00A95735,0x00AE5836,0x00A55333,0x009D4E30,0x009C4C2F,0x009A4B2E,0x009E4C2F,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009A4B2F,0x00AB5535,0x00A95935,0x00B05F39,0x00B7653C,0x00BC6B3E,0x00C06F41,0x00C97444,0x00CF7846,0x00C37041,0x00C57042,0x00000000,0x00000000,0x00000000,0x00C67343,0x00CD7645,0x00C77143,0x00BB693E,0x00B5643B,0x00B05E39,0x00AA5A36,0x00A65534,0x00A15131,0x00AA5434,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A55334,0x00B4603A,0x00B8663D,0x00BF6D40,0x00C67343,0x00CC7946,0x00D17E48,0x00D5824A,0x00D7844C,0x00D9854C,0x00DD844C,0x00BB683D,0x00B4643A,0x00D77F49,0x00D9844C,0x00D6834B,0x00D5814A,0x00CE7C47,0x00C77444,0x00BF6D40,0x00B7663C,0x00B15F39,0x00AB5A36,0x00AF5A37,0x00A15031,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00AE5D37,0x00BE6B3F,0x00C57343,0x00CC7946,0x00D17E49,0x00D6834B,0x00DA874D,0x00DE8A4F,0x00E38F51,0x00D9864C,0x00C16E41,0x00B4623B,0x00B4613A,0x00B3623A,0x00C97645,0x00DF8B4F,0x00E08C50,0x00D9864D,0x00D4804A,0x00CC7A46,0x00C47243,0x00BC6B3F,0x00B5633B,0x00B5613A,0x00A55533,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00B8663C,0x00C87544,0x00D07D48,0x00D5824B,0x00DA874D,0x00DD8A4E,0x00E18E50,0x00E28E51,0x00D17F49,0x00C06E40,0x00BE6C40,0x00C06E40,0x00BD6C3F,0x00BA683E,0x00B7663C,0x00C47242,0x00DC894E,0x00E38F51,0x00DD894E,0x00D7844B,0x00D17E48,0x00C97645,0x00C06E40,0x00BD693E,0x00AC5A36,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00BF6C40,0x00D07D48,0x00D8854C,0x00DC894E,0x00E08C4F,0x00E49051,0x00E08C4F,0x00D07D48,0x00CA7745,0x00CD7B47,0x00CD7B47,0x00CB7946,0x00CA7745,0x00C87644,0x00C77544,0x00C27142,0x00C37142,0x00D5824B,0x00E39051,0x00DF8C4F,0x00DA864D,0x00D4804A,0x00CC7946,0x00C77243,0x00B3623A,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00C67243,0x00D7844B,0x00DE8B4F,0x00E18E50,0x00E59152,0x00DE8B4F,0x00D3804A,0x00D5824A,0x00D7844C,0x00D6824B,0x00D4814A,0x00D4814A,0x00D4814A,0x00D4814A,0x00D3804A,0x00D27F49,0x00CE7B47,0x00C87644,0x00D17F49,0x00E28E51,0x00E18D50,0x00DC884D,0x00D6834B,0x00D17C48,0x00BB693D,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00C97744,0x00DD884E,0x00E38F51,0x00E59152,0x00DE8B4F,0x00D9864C,0x00DD8A4E,0x00DD8A4E,0x00DB884D,0x00DA864D,0x00D9854C,0x00D8854C,0x00D9854C,0x00DA864D,0x00DB884D,0x00DC884E,0x00DB874D,0x00D7844B,0x00CF7C48,0x00D17E49,0x00E18D50,0x00E28E51,0x00DD8A4E,0x00DB854C,0x00C47142,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00CC7946,0x00E08B4F,0x00E49152,0x00DF8B4F,0x00DD8A4E,0x00E28E51,0x00E18E50,0x00E08C50,0x00DF8B4F,0x00DE8A4F,0x00DD8A4E,0x00DD8A4E,0x00DD8A4E,0x00DE8B4F,0x00DF8C4F,0x00E18D50,0x00E28E50,0x00E18D50,0x00DD894E,0x00D4824B,0x00D3804A,0x00E18D50,0x00E28F50,0x00E38B50,0x00CA7745,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00CE7B46,0x00E38C50,0x00E08D50,0x00DF8C50,0x00E59052,0x00E49052,0x00E29053,0x00E28F51,0x00E28E51,0x00E18E50,0x00E18D50,0x00E18D50,0x00E18E50,0x00E28E51,0x00E38F51,0x00E38F51,0x00E49051,0x00E49153,0x00E39153,0x00E18C50,0x00DA864D,0x00D9864D,0x00E28F51,0x00EA9153,0x00D17E48,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00CD7A47,0x00E1874C,0x00DF884B,0x00E48E4D,0x00E19153,0x00E89151,0x00ED8E4C,0x00E79050,0x00E49153,0x00E59052,0x00E49052,0x00E49052,0x00E49052,0x00E49052,0x00E59152,0x00E59152,0x00E49153,0x00EA8F4D,0x00EC8E4D,0x00E49254,0x00E08E50,0x00DE894B,0x00DD894B,0x00EB9151,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00F1975D,0x00ED9F66,0x00F29F62,0x00BB9A73,0x0091A590,0x00B1A780,0x00DF9457,0x00EA9150,0x00E49153,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E59253,0x00EB904F,0x00CD9D6A,0x00A9B493,0x00A0A88D,0x00DB945D,0x00F09E62,0x00F2A165,0x00F39B5D,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00F2E6DC,0x00F4ECE6,0x00FAEFE8,0x00F1F5EE,0x00C4E1DA,0x00A0B6A1,0x00D38C50,0x00E89151,0x00E49153,0x00E59152,0x00E59152,0x00E59152,0x00E59355,0x00E88E4D,0x00B2966D,0x00A3C6BA,0x00D7EAE1,0x00FFF9EF,0x00FAEFE7,0x00F7EEE8,0x00F3E3D6,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00FFFFFF,0x00FAFDFB,0x00DEC9B0,0x00E38844,0x00E38F50,0x00E59253,0x00E59151,0x00E59355,0x00E68C48,0x00D7945D,0x00DBE3DB,0x00FFFFFF,0x00FFFFFF,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00FFFFFF,0x00FADBC5,0x00EA9454,0x00E28D4D,0x00E39255,0x00E28946,0x00EEA46D,0x00FFF4EA,0x00FFFFFF,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00DAD9D7,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A85534,0x00A65533,0x00AC5836,0x00B15C39,0x00B15F3B,0x00B05E39,0x00000000,0x00000000,0x00000000,0x00F2D2BD,0x00F49D5C,0x00F2954F,0x00F6AB74,0x00F9EFE9,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00DCDCDC,0x00CDCDCD,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009F4F31,0x00AE5736,0x009E4E31,0x009F4F31,0x00A25232,0x00A75735,0x00B15E3A,0x00C06940,0x00BB683E,0x00000000,0x00000000,0x00CC9F80,0x00A0663B,0x00F9DFCA,0x00000000,0x00000000,0x00E29052,0x00E89354,0x00E99454,0x00EA9353,0x00000000,0x00000000,0x00000000,0x00CBCBCB,0x00F2F1F1,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00AA5334,0x009A4A2F,0x009B4B2F,0x009D4D30,0x00A15132,0x00A65634,0x00AE5C38,0x00B3623B,0x00BE6A3F,0x00C36F43,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00E19052,0x00FC9F5A,0x00E79353,0x00E49051,0x00FFA35C,0x00DA874C,0x00000000,0x00000000,0x00D5D4D4,0x00ECEBEB,0x00DEDDDD,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009C4A2F,0x00A54F31,0x0098492D,0x0099492E,0x009C4C2F,0x00A05031,0x00A65634,0x00AE5D38,0x00B8673D,0x00C16F41,0x00D07946,0x00C67444,0x00000000,0x00000000,0x00000000,0x00000000,0x00F69C58,0x00E59052,0x00E59152,0x00E69151,0x00E1894E,0x00000000,0x00000000,0x00000000,0x00DCDBDB,0x00E3E2E2,0x00E3E1E1,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009A492D,0x00A24C30,0x0097472C,0x0098482E,0x009B4B2F,0x00A05031,0x00A75634,0x00AF5E39,0x00BA683E,0x00C67444,0x00D07C48,0x00DE854D,0x00C97945,0x00000000,0x00000000,0x00E18F50,0x00FDA15B,0x00DF8B4F,0x00EC9353,0x00E68D51,0x00000000,0x00000000,0x00000000,0x00E3E2E2,0x00EAE9E9,0x00EAE8E8,0x00F0EEEE,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A14C30,0x0095462C,0x0097482D,0x009B4B2F,0x00A05031,0x00A75735,0x00B15F3A,0x00BC6A3F,0x00C87644,0x00D4814A,0x00DC884D,0x00DC864D,0x00000000,0x00000000,0x00000000,0x00E08B4F,0x00D17E48,0x00D5814B,0x00000000,0x00000000,0x00000000,0x00000000,0x00F2F1F1,0x00E5E4E3,0x00FEFDFD,0x00F9F7F7,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0097462C,0x0096462C,0x0097482D,0x009B4B2F,0x00A15132,0x00A95835,0x00B2613A,0x00BD6B3F,0x00CA7745,0x00D5824A,0x00DD894E,0x00E28E50,0x00DB854C,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00F1F0F0,0x00EBEAE9,0x00EDEDEE,0x00FAF8F9,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0093442B,0x00A44D31,0x0097482D,0x009D4D30,0x00A35232,0x00AA5936,0x00B3623B,0x00BE6C40,0x00C97745,0x00D4814A,0x00DD894E,0x00E18E50,0x00E48F51,0x00D9834A,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00F3F0EE,0x00F8F5F4,0x00EAE8E7,0x00EAEEF2,0x00FFFFFB,0x00EECDB7,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0099472D,0x00A54F32,0x009D4D2F,0x00A45333,0x00AB5A37,0x00B4633B,0x00BE6C3F,0x00C87544,0x00D17F49,0x00DA874D,0x00E18D50,0x00E38F52,0x00E28E50,0x00E38C51,0x00E2AB87,0x00F5E8DF,0x00F5F7FB,0x00FFFFFF,0x00F7FBFF,0x00EFF5F9,0x00EFF3F7,0x00FFEDDF,0x00E8B28C,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009E4C30,0x00AD5636,0x00A65534,0x00AB5B36,0x00B4633A,0x00BB6A3E,0x00C47243,0x00CE7B47,0x00D7844B,0x00DE8B4F,0x00E38F51,0x00E49152,0x00E28E4F,0x00E18747,0x00E79559,0x00E8AC80,0x00EBC4A6,0x00EECEB6,0x00EEC6AA,0x00F3B489,0x00DE925C,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A55334,0x00AD5936,0x00B9623B,0x00BA653C,0x00BB693E,0x00BF6E40,0x00CA7745,0x00D3804A,0x00DC884E,0x00E28E50,0x00E59152,0x00E59152,0x00E59153,0x00E38E4F,0x00E28947,0x00E88A47,0x00E68847,0x00DA7E40,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00B15E39,0x00B25E39,0x00B3613B,0x00BE6A3F,0x00CC7444,0x00D57D49,0x00DD844C,0x00E38A4F,0x00E68E51,0x00E88F52,0x00E78E51,0x00E0894F,0x00D6834D,0x00D5834E,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00BF6B3F,0x00C37142,0x00C77544,0x00CE7A46,0x00D07C47,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
};

static const uint32_t fox_logo_60[60 * 60] = {
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0093432A,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0099432C,0x00904029,0x0092422A,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0097462D,0x0096442B,0x009A472D,0x00A74D31,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x008F3F29,0x0099452D,0x0095462C,0x00A04C30,0x00A54F32,0x009D4D30,0x00A35233,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A04E31,0x009D4B2F,0x00A24D31,0x00A04C30,0x0099492E,0x0096472C,0x009C492E,0x0096452D,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009A482E,0x009A4A2E,0x009F4F31,0x00A35232,0x00A75734,0x00AB5A37,0x00B45F3A,0x00B9633C,0x00B2603A,0x00B7643B,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00B9663B,0x00B05E38,0x00B25C38,0x00AF5937,0x00A55434,0x00A05031,0x009E4E30,0x009D4D30,0x009C4C30,0x009A4A2F,0x00A24E31,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0099492E,0x00A85233,0x00A45333,0x00AA5A36,0x00AF5E39,0x00B5633B,0x00B9683D,0x00BE6C3F,0x00BF6E40,0x00C37042,0x00CC7544,0x00C57042,0x00C26F41,0x00000000,0x00000000,0x00000000,0x00000000,0x00C77344,0x00C47142,0x00CB7444,0x00C16D40,0x00BA683D,0x00B6653C,0x00B2613A,0x00AE5D38,0x00AA5936,0x00A65634,0x00A35333,0x00A05030,0x00A95334,0x009C4A2E,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A05032,0x00AF5A37,0x00B05F38,0x00B6653C,0x00BC6A3E,0x00C16F41,0x00C67443,0x00CB7845,0x00CF7C47,0x00D17E49,0x00D27F49,0x00CF7D48,0x00D37D48,0x00D57D48,0x00C06E41,0x00BB6A3D,0x00CA7645,0x00D8804A,0x00D07D48,0x00D28049,0x00D07D48,0x00CC7946,0x00C67443,0x00C06E40,0x00BA683D,0x00B4633B,0x00AF5E38,0x00AA5A36,0x00A65634,0x00AA5635,0x009F4E30,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00AA5835,0x00B5623B,0x00BB693E,0x00C16F41,0x00C67444,0x00CB7946,0x00D07D48,0x00D4814A,0x00D7844C,0x00DA874D,0x00DD894E,0x00E08D50,0x00DB894D,0x00C57342,0x00B8633B,0x00B7623B,0x00B9673D,0x00CD7B46,0x00DB884D,0x00DC894E,0x00DA874D,0x00D7834B,0x00D17E49,0x00CB7846,0x00C57343,0x00BE6C40,0x00B8673D,0x00B3623B,0x00AD5D38,0x00AD5A37,0x00A25233,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00B05F38,0x00BD6A3F,0x00C57343,0x00CB7846,0x00CF7D48,0x00D4814A,0x00D8844C,0x00DB874D,0x00DD8A4E,0x00E08C50,0x00E49051,0x00D9854C,0x00C16F41,0x00B7653C,0x00B7653C,0x00B5643B,0x00B1603A,0x00B4623B,0x00C77544,0x00DE8B4F,0x00E28E51,0x00DD8A4E,0x00DA864D,0x00D5824B,0x00CF7C48,0x00C97745,0x00C37142,0x00BC6B3F,0x00B6653C,0x00B26039,0x00A75735,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00B7653C,0x00C47142,0x00CE7B47,0x00D38049,0x00D7834B,0x00DA874D,0x00DD8A4E,0x00DF8B4F,0x00E38F51,0x00E28E51,0x00D07E48,0x00C06E41,0x00C06E40,0x00C27041,0x00C16F41,0x00BF6D40,0x00BD6C3F,0x00BB6A3E,0x00B8673D,0x00C16F41,0x00D9854C,0x00E49052,0x00E08C50,0x00DC884E,0x00D8844C,0x00D27F49,0x00CC7946,0x00C67343,0x00BF6D40,0x00B8663C,0x00AD5C37,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00BE6C40,0x00CA7745,0x00D5814A,0x00D9864C,0x00DC894E,0x00DF8B4F,0x00E18D50,0x00E49052,0x00DE8B4F,0x00CD7B47,0x00C87645,0x00CC7A46,0x00CD7A46,0x00CB7946,0x00CA7845,0x00C97645,0x00C77544,0x00C67443,0x00C57343,0x00C27041,0x00C16F41,0x00D17E49,0x00E38F51,0x00E28E51,0x00DD8A4E,0x00DA864D,0x00D5824A,0x00CF7C48,0x00C87644,0x00BF6D40,0x00B4623B,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00C27042,0x00D07D48,0x00DA874D,0x00DE8A4F,0x00E08D50,0x00E28F51,0x00E59152,0x00DC884E,0x00D07D48,0x00D27F49,0x00D5824A,0x00D4814A,0x00D3804A,0x00D38049,0x00D27F49,0x00D27F49,0x00D17E49,0x00D07E48,0x00CF7C48,0x00CD7B47,0x00CB7846,0x00C67443,0x00CD7A46,0x00E08C50,0x00E49051,0x00DF8B4F,0x00DB884E,0x00D7834B,0x00D17E49,0x00C77544,0x00B9683D,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00C67343,0x00D5814A,0x00DF8B4F,0x00E28E50,0x00E39051,0x00E59152,0x00DB884E,0x00D6824B,0x00DA874D,0x00DB874D,0x00D9864D,0x00D8854C,0x00D7844B,0x00D6834B,0x00D6834B,0x00D6834B,0x00D7844B,0x00D8844C,0x00D8854C,0x00D7844B,0x00D5824A,0x00D27F49,0x00CD7A46,0x00CC7A46,0x00DD894E,0x00E49052,0x00E08C50,0x00DD894E,0x00D8854C,0x00CF7C47,0x00C06E40,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00C97845,0x00D9844C,0x00E18D50,0x00E49052,0x00E49052,0x00DC884E,0x00DA874D,0x00DF8B4F,0x00DF8B4F,0x00DE8A4F,0x00DC894E,0x00DB884D,0x00DA874D,0x00DA874D,0x00DA864D,0x00DA874D,0x00DB874D,0x00DC884E,0x00DD894E,0x00DE8A4F,0x00DE8A4F,0x00DB884D,0x00D8844C,0x00D27F49,0x00CE7B47,0x00DC884E,0x00E49152,0x00E18D50,0x00DC894E,0x00D7824B,0x00C57342,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00CC7946,0x00DC864D,0x00E28F50,0x00E59152,0x00DD894E,0x00DD8A4E,0x00E28F51,0x00E28E51,0x00E18D50,0x00E08C50,0x00DF8C4F,0x00DF8B4F,0x00DE8A4F,0x00DE8A4F,0x00DE8A4F,0x00DE8A4F,0x00DE8B4F,0x00DF8B4F,0x00E08C50,0x00E18D50,0x00E28E51,0x00E28E51,0x00E08C4F,0x00DC884E,0x00D6834B,0x00D17E49,0x00DC894E,0x00E59152,0x00E08D4F,0x00DE884E,0x00CB7945,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00CD7B47,0x00DE874E,0x00E18D4F,0x00DF8B4F,0x00DF8B4F,0x00E39053,0x00E39053,0x00E39052,0x00E38F51,0x00E28E51,0x00E28E50,0x00E18D50,0x00E18D50,0x00E18D50,0x00E18D50,0x00E18D50,0x00E18E50,0x00E28E51,0x00E38F51,0x00E38F51,0x00E49051,0x00E49052,0x00E49052,0x00E28F52,0x00DD8C50,0x00DA874D,0x00D6834B,0x00DF8B4F,0x00E38F50,0x00E48D50,0x00D07D48,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00CD7A46,0x00E0884E,0x00DB884E,0x00E18E52,0x00E49254,0x00EA9150,0x00EC8F4C,0x00E78F4F,0x00E49153,0x00E49153,0x00E49052,0x00E49052,0x00E39051,0x00E38F51,0x00E49051,0x00E49051,0x00E49051,0x00E49052,0x00E49052,0x00E59152,0x00E59152,0x00E49253,0x00E59152,0x00E88E4D,0x00EB8D4B,0x00E38E50,0x00DE8B51,0x00DC8A4F,0x00E08D51,0x00EA9153,0x00D48249,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00DC8249,0x00D78043,0x00E28A48,0x00E68B46,0x00CB905B,0x00C09A6E,0x00DB9961,0x00EB904E,0x00E88F4F,0x00E49253,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E49153,0x00EA8E4C,0x00E79253,0x00D69E69,0x00C09D71,0x00D88E52,0x00E38846,0x00DF8746,0x00DE8647,0x00EA904F,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00E2905C,0x00ECA778,0x00F6B687,0x00F3B181,0x00CCA885,0x008BACA1,0x0087B6A8,0x00A0B498,0x00D89860,0x00EB904F,0x00E49153,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E59253,0x00EC904E,0x00C7A071,0x009EBEA6,0x0093C4B3,0x00A3AF9A,0x00DFA373,0x00F0AE7D,0x00F6B483,0x00F5AD7A,0x00EC9C63,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00F8FBFF,0x00F6FAFD,0x00FFFCFB,0x00FFFFFB,0x00F0EDE5,0x00B9D1C7,0x0091B5A3,0x00CD9059,0x00E99151,0x00E49253,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E59152,0x00E49254,0x00EA904E,0x00B1956D,0x0091BCB0,0x00C8D7CA,0x00F4EEE4,0x00FFFFFD,0x00FFFFFF,0x00F9FCFF,0x00F9FCFD,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00FFFFFF,0x00FFFFFF,0x00FDFEFA,0x00C4BAA4,0x00E18744,0x00E69151,0x00E59253,0x00E59152,0x00E59152,0x00E59152,0x00E59254,0x00E88F4D,0x00D18D55,0x00BBC6BC,0x00FFFFFA,0x00FFFFFF,0x00FFFFFF,0x00FDFFFF,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00FFFFFF,0x00FFFBFB,0x00F9D7BF,0x00E28C4C,0x00E38E4E,0x00E59254,0x00E59151,0x00E59354,0x00E38D4C,0x00E49255,0x00FCE6D4,0x00FFFFFF,0x00FFFFFF,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00DBDBDB,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A95534,0x00A75634,0x00AA5937,0x00AE5D3A,0x00B05E38,0x00000000,0x00000000,0x00000000,0x00FBFFFF,0x00FAE4D4,0x00E89456,0x00E38D4C,0x00E39154,0x00E38C4A,0x00E8995E,0x00FFF3E8,0x00FFFFFF,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00D8D8D8,0x00DEDDDD,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A65434,0x00AE5836,0x00AB5635,0x00A95534,0x00AA5735,0x00B05B38,0x00B8613B,0x00BA643E,0x00B5623C,0x00000000,0x00000000,0x00000000,0x00F2D4C0,0x00F49C5B,0x00F49752,0x00F4A163,0x00F7E5D9,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00D3D2D2,0x00D8D8D8,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A04F32,0x00AB5535,0x009D4D2F,0x009D4D30,0x00A04F31,0x00A25232,0x00A55634,0x00A95935,0x00AE5D37,0x00B7633B,0x00C36C41,0x00B96841,0x00000000,0x00000000,0x00C79D82,0x00794824,0x00E8C5AD,0x00000000,0x00000000,0x00000000,0x00E48F54,0x00E79252,0x00F39957,0x00F39A57,0x00EB9453,0x00000000,0x00000000,0x00000000,0x00000000,0x00CDCCCC,0x00DFDEDE,0x00D9D8D8,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00AB5434,0x009B4B2F,0x009C4C2F,0x009C4C30,0x009E4E31,0x00A15132,0x00A55433,0x00AA5936,0x00B05E39,0x00B3623B,0x00B7663C,0x00CC7344,0x00BC6C42,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00E48F51,0x00F89D59,0x00E49152,0x00E49051,0x00E49051,0x00F79D59,0x00DB874D,0x00000000,0x00000000,0x00000000,0x00D3D2D2,0x00D8D7D7,0x00E1E0E0,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009C4C2F,0x00A44F31,0x0099492E,0x0099492E,0x009A4B2F,0x009C4C30,0x00A04F31,0x00A45433,0x00AA5936,0x00B15F3A,0x00B8673D,0x00BF6D40,0x00C27041,0x00CF7846,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00E28D50,0x00FA9E59,0x00E49052,0x00E59152,0x00E69252,0x00E38F51,0x00F09555,0x00D3814A,0x00000000,0x00000000,0x00000000,0x00DCDBDB,0x00D6D5D5,0x00EBEAEA,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0099492E,0x009F4C30,0x0097472D,0x0098482D,0x0099492E,0x009B4B2F,0x009F4F31,0x00A45433,0x00AA5936,0x00B1603A,0x00BA683E,0x00C47242,0x00CA7745,0x00D27C48,0x00CB7845,0x00000000,0x00000000,0x00000000,0x00000000,0x00E39051,0x00EC9555,0x00E38F50,0x00E28F50,0x00E59152,0x00E28B4F,0x00CD7845,0x00000000,0x00000000,0x00000000,0x00E0DEDE,0x00EAE9E9,0x00DBD9D9,0x00F3F1F1,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0099482D,0x009F4B30,0x0096462C,0x0097472D,0x0098492E,0x009B4B2F,0x009F4F31,0x00A45433,0x00AB5A36,0x00B3613A,0x00BB6A3F,0x00C57343,0x00D07D48,0x00D38149,0x00E0874E,0x00CE7B47,0x00000000,0x00000000,0x00000000,0x00E18E50,0x00F39B57,0x00DF8B4F,0x00E1894E,0x00ED9253,0x00D27D47,0x00000000,0x00000000,0x00000000,0x00000000,0x00EAE9E9,0x00E2E1E1,0x00E2E0E0,0x00F6F4F4,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0096462D,0x00A14B30,0x0095462C,0x0096472D,0x0098482E,0x009B4B2F,0x009F4F31,0x00A55533,0x00AC5B37,0x00B4633B,0x00BD6B3F,0x00C77544,0x00D17E48,0x00D9864C,0x00DB884D,0x00E28A4F,0x00000000,0x00000000,0x00000000,0x00000000,0x00DC864D,0x00D47F48,0x00C97644,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00EEEDED,0x00EDECEC,0x00E6E5E5,0x00F4F2F1,0x00F9F6F6,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009A482E,0x0095452C,0x0096462D,0x0098492E,0x009C4C2F,0x00A05031,0x00A65534,0x00AD5C38,0x00B5643C,0x00BE6C40,0x00C87644,0x00D27F49,0x00DA864D,0x00DF8B4F,0x00DF8B4F,0x00DE874E,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00F1EFEF,0x00EFEEEE,0x00E9E8E8,0x00E9E6E6,0x00FFFFFF,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0095452C,0x00A04B2F,0x0096462C,0x009A492E,0x009D4D30,0x00A15132,0x00A75635,0x00AE5D38,0x00B6653C,0x00BF6D40,0x00C97645,0x00D17E49,0x00D9864D,0x00E08C4F,0x00E18E50,0x00DF8B4F,0x00DA854C,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00FAF6F6,0x00F2F1F1,0x00EDECEC,0x00EAE8E8,0x00E7E6E6,0x00F9FBFE,0x00F5E3D8,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009A482D,0x009C4A2E,0x0099492D,0x009E4E30,0x00A35233,0x00A85836,0x00AF5E39,0x00B7653C,0x00BF6D40,0x00C87544,0x00D07D48,0x00D8844C,0x00DE8A4F,0x00E28E51,0x00E28F51,0x00DE8B4F,0x00DF874C,0x00DA986F,0x00F3EAE5,0x00000000,0x00F9F9FB,0x00F9F5F4,0x00F4F0EF,0x00F8F6F5,0x00E9E8E7,0x00ECEAEA,0x00EBECEF,0x00F5F7FB,0x00F6D7C1,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x009D4B2F,0x00A14E31,0x009E4E30,0x00A45333,0x00A95836,0x00B05F39,0x00B7653C,0x00BE6C40,0x00C67343,0x00CE7B47,0x00D5824A,0x00DC884E,0x00E18D50,0x00E49052,0x00E49052,0x00DE8B4F,0x00E1884B,0x00EEA370,0x00FFDCC3,0x00FFFDF9,0x00FAFCFF,0x00EDF1F6,0x00F0F5FA,0x00F3F8FC,0x00EFF2F5,0x00F2E4DB,0x00F3BE99,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A14E31,0x00AB5535,0x00A45433,0x00AA5A36,0x00B05F39,0x00B6653C,0x00BC6B3F,0x00C37142,0x00CA7845,0x00D27F49,0x00D9864C,0x00DF8B4F,0x00E38F51,0x00E59152,0x00E59152,0x00E38F51,0x00DF8849,0x00DF8542,0x00E39358,0x00E7AC7E,0x00EDC09F,0x00EBC4A6,0x00E6BA98,0x00EFAC7E,0x00E1915A,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00A65433,0x00B25C38,0x00B35F39,0x00B15F39,0x00B4633A,0x00B9683D,0x00C06E40,0x00C77444,0x00CE7C47,0x00D6834B,0x00DC894E,0x00E18D50,0x00E49052,0x00E59152,0x00E59152,0x00E59153,0x00E49254,0x00E28D4E,0x00E08746,0x00DF8544,0x00E68846,0x00DF8243,0x00D97E41,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00B15E39,0x00B05D38,0x00BB653C,0x00C16A3F,0x00C36D41,0x00C57142,0x00C97645,0x00CF7C47,0x00D6824A,0x00DA874C,0x00DD8A4E,0x00DF8B4F,0x00DD8A4F,0x00DF8A4F,0x00E18B4F,0x00E58D51,0x00E28B51,0x00D5834D,0x00D8844E,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00B26039,0x00B6633B,0x00B9683D,0x00BD6B3E,0x00C97444,0x00D27C48,0x00D8814A,0x00DC844C,0x00DC854C,0x00D7824B,0x00D27E48,0x00D47F49,0x00D5804A,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
};

static void draw_fox_face(int x, int y, int size)
{
    const uint32_t *data;
    int source_size;

    if (size <= 24) {
        data = fox_logo_20;
        source_size = 20;
    } else if (size <= 40) {
        data = fox_logo_28;
        source_size = 28;
    } else if (size <= 54) {
        data = fox_logo_48;
        source_size = 48;
    } else {
        data = fox_logo_60;
        source_size = 60;
    }

    uint32_t sw = fb_get_width(), sh = fb_get_height();
    for (int row = 0; row < size; row++) {
        int py = y + row;
        if (py < 0 || py >= (int)sh) continue;
        int src_y = row * source_size / size;
        for (int col = 0; col < size; col++) {
            int px = x + col;
            if (px < 0 || px >= (int)sw) continue;
            int src_x = col * source_size / size;
            uint32_t pixel = data[src_y * source_size + src_x];
            if (pixel == 0x00000000) continue;
            fb_put_pixel(px, py, (fb_color_t)pixel);
        }
    }
}

/* ================================================================
 *  WINDOW MANAGEMENT
 * ================================================================ */

static void compute_app_size(int app, int *w, int *h)
{
    uint32_t sw = fb_get_width(), sh = fb_get_height();
    switch (app) {
    case 0: *w = sw > 720 ? 700 : (int)sw - 40;
            *h = sh > 520 ? 480 : (int)sh - 100; break;
    case 1: *w = sw > 520 ? 500 : (int)sw - 40;
            *h = sh > 420 ? 380 : (int)sh - 100; break;
    case 2: *w = sw > 520 ? 500 : (int)sw - 40;
            *h = sh > 520 ? 480 : (int)sh - 100; break;
    case 3: *w = sw > 420 ? 400 : (int)sw - 40;
            *h = 360; break;
    case 4: *w = sw > 420 ? 400 : (int)sw - 40;
            *h = 300; break;
    case 5: *w = sw > 420 ? 400 : (int)sw - 40;
            *h = 300; break;
    case 6: *w = sw > 320 ? 300 : (int)sw - 40;
            *h = 220; break;
    case 7: *w = MS_COLS * MS_CELL_SIZE + 40;
            *h = MS_ROWS * MS_CELL_SIZE + TITLE_BAR_HEIGHT + 64; break;
    default: *w = (int)sw - 40;
             *h = (int)sh - TASKBAR_HEIGHT - 60; break;
    }
}

static int open_window(int app)
{
    if (num_wins >= MAX_WINDOWS) return -1;
    WinSlot *ws = &wins[num_wins];
    ws->app = app;
    ws->minimized = 0;
    ws->maximized = 0;
    compute_app_size(app, &ws->w, &ws->h);
    int offset = num_wins * 32;
    ws->x = ((int)fb_get_width() - ws->w) / 2 + offset;
    ws->y = ((int)fb_get_height() - TASKBAR_HEIGHT - DOCK_FLOAT_GAP - ws->h) / 2 + offset;
    if (ws->x + ws->w > (int)fb_get_width()) ws->x = 0;
    if (ws->y + ws->h > dock_y()) ws->y = 0;
    focus_win = num_wins;
    num_wins++;
    return focus_win;
}

static void close_window(int slot)
{
    if (slot < 0 || slot >= num_wins) return;
    for (int i = slot; i < num_wins - 1; i++)
        wins[i] = wins[i + 1];
    num_wins--;
    if (focus_win >= num_wins) focus_win = num_wins - 1;
    if (drag_win == slot) { dragging = 0; drag_win = -1; }
    else if (drag_win > slot) drag_win--;
    if (resize_win == slot) { resizing = 0; resize_win = -1; }
    else if (resize_win > slot) resize_win--;
}

static int get_resize_edge(WinSlot *ws, int mx, int my)
{
    if (ws->maximized) return 0;
    int edge = 0;
    int margin = 8;
    if (mx >= ws->x + ws->w - margin && mx < ws->x + ws->w + 4 &&
        my >= ws->y && my < ws->y + ws->h)
        edge |= 1;
    if (my >= ws->y + ws->h - margin && my < ws->y + ws->h + 4 &&
        mx >= ws->x && mx < ws->x + ws->w)
        edge |= 2;
    if (mx >= ws->x - 4 && mx < ws->x + margin &&
        my >= ws->y && my < ws->y + ws->h)
        edge |= 4;
    if (my >= ws->y - 4 && my < ws->y + margin &&
        mx >= ws->x && mx < ws->x + ws->w)
        edge |= 8;
    return edge;
}

/* ================================================================
 *  SPLASH SCREEN
 * ================================================================ */

static void show_splash(void)
{
    uint32_t sw = fb_get_width(), sh = fb_get_height();
    uint32_t pitch = fb_get_pitch();

    /* Dark gradient background - optimized with per-scanline fill */
    uint32_t *buf = (uint32_t *)fb_get_buffer();
    int stride = pitch / 4;
    for (int y = 0; y < (int)sh; y++) {
        int t = y * 256 / (int)sh;
        int r = 0x0A + ((0x1A - 0x0A) * t >> 8);
        int g = 0x0A + ((0x10 - 0x0A) * t >> 8);
        int b = 0x1A + ((0x40 - 0x1A) * t >> 8);
        fb_color_t c = (r << 16) | (g << 8) | b;
        uint32_t *line = buf + y * stride;
        for (int x = 0; x < (int)sw; x++) {
            line[x] = c;
        }
    }

    /* Logo */
    int logo_x = ((int)sw - LOGO_WIDTH) / 2;
    int logo_y = ((int)sh - LOGO_HEIGHT) / 2 - 30;
    for (int y = 0; y < LOGO_HEIGHT; y++)
        for (int x = 0; x < LOGO_WIDTH; x++) {
            fb_color_t pixel = logo_pixels[y * LOGO_WIDTH + x];
            if (pixel != 0x000000)
                fb_put_pixel(logo_x + x, logo_y + y, pixel);
        }

    /* Title text */
    fb_draw_string(((int)sw - 11 * 8) / 2, logo_y + LOGO_HEIGHT + 20,
                   "SpiritFoxOS", COLOR_TEXT_DIM, COLOR_BG_TL);

    /* Loading dots animation */
    for (int i = 0; i < 3; i++) {
        int dot_x = ((int)sw) / 2 - 20 + i * 16;
        int dot_y = logo_y + LOGO_HEIGHT + 44;
        fill_rounded_rect(dot_x, dot_y, 8, 8, 4, COLOR_ACCENT);
    }

    fb_swap_buffer();
    timer_sleep_ms(3000);
}

/* ================================================================
 *  MAIN ENTRY
 * ================================================================ */

static void restore_vga_text_mode(void)
{
    fb_term_init();
    printf("Returned from graphical mode.\n/> ");
}

void window_enter(void)
{
    if (!g_boot_info) g_boot_info = (BootInfo *)0x9000;
    fb_init(g_boot_info);
    if (fb_get_width() == 0 || fb_get_height() == 0) return;

    mouse_init();

    /* Show splash */
    show_splash();

    term_clear();
    term_print("SpiritFoxOS Terminal v0.5\n");
    term_print("Type 'help' for commands.\n\n");
    term_input[0] = '\0';
    term_input_len = 0;

    int window_running = 1;
    num_wins = 0;
    focus_win = -1;
    start_menu_open = 0;
    ctx_menu_open = 0;
    icon_pressed = -1;
    anim_tick = 0;

    { uint8_t btn;
      mouse_get_state(&mouse_x, &mouse_y, &btn);
      mouse_prev_x = mouse_x; mouse_prev_y = mouse_y;
      mouse_prev_buttons = btn; mouse_buttons = btn; }

    /* ======== MAIN LOOP ======== */
    while (window_running) {
        anim_tick++;

        /* ---- Keyboard ---- */
        while (keyboard_has_char()) {
            char c = keyboard_get_char();
            if ((unsigned char)c == 0x1B) { window_running = 0; break; }

            /* Alt+Tab: cycle windows */
            if (c == '\t' && num_wins > 1) {
                focus_win = (focus_win + 1) % num_wins;
                wins[focus_win].minimized = 0;
                continue;
            }

            /* Terminal input */
            if (focus_win >= 0 && wins[focus_win].app == 0) {
                if (c == '\n') {
                    term_input[term_input_len] = '\0';
                    term_execute(term_input);
                    term_input_len = 0;
                    term_input[0] = '\0';
                } else if (c == '\b' || c == 0x7F) {
                    if (term_input_len > 0) term_input[--term_input_len] = '\0';
                } else if (c >= 0x20 && c < 0x7F) {
                    if (term_input_len < TERM_BUF_COLS - 1)
                        term_input[term_input_len++] = c;
                    term_input[term_input_len] = '\0';
                }
            }
        }
        if (!window_running) break;

        /* ---- Mouse ---- */
        mouse_prev_x = mouse_x; mouse_prev_y = mouse_y;
        mouse_prev_buttons = mouse_buttons;
        if (mouse_has_event())
            mouse_get_state(&mouse_x, &mouse_y, &mouse_buttons);

        int left_click = (mouse_buttons & MOUSE_BTN_LEFT) &&
                         !(mouse_prev_buttons & MOUSE_BTN_LEFT);
        int left_down = (mouse_buttons & MOUSE_BTN_LEFT) != 0;
        int right_click = (mouse_buttons & MOUSE_BTN_RIGHT) &&
                          !(mouse_prev_buttons & MOUSE_BTN_RIGHT);

        /* 双击检测 */
        if (left_click) {
            uint64_t now = timer_get_ms();
            if (now - last_click_time < 400 &&
                mouse_x >= last_click_x - 4 && mouse_x <= last_click_x + 4 &&
                mouse_y >= last_click_y - 4 && mouse_y <= last_click_y + 4) {
                double_click = 1;
            } else {
                double_click = 0;
            }
            last_click_time = now;
            last_click_x = mouse_x;
            last_click_y = mouse_y;
        } else {
            double_click = 0;
        }

        int tb_y = dock_y();

        /* ---- Icon press tracking ---- */
        if (!left_down) icon_pressed = -1;

        /* ---- Start menu hover ---- */
        if (start_menu_open)
            start_menu_hover = start_menu_item_at(mouse_x, mouse_y);
        else
            start_menu_hover = -1;

        /* ---- Context menu hover ---- */
        if (ctx_menu_open)
            ctx_menu_hover = ctx_menu_item_at(mouse_x, mouse_y);
        else
            ctx_menu_hover = -1;

        /* ---- Taskbar / Dock click ---- */
        if (left_click && mouse_y >= tb_y) {
            /* Start button */
            int sx, sy, sw2, sh2;
            dock_start_rect(&sx, &sy, &sw2, &sh2);
            if (mouse_x >= sx && mouse_x < sx + sw2 &&
                mouse_y >= sy && mouse_y < sy + sh2) {
                start_menu_open = !start_menu_open;
                if (!start_menu_open) start_menu_hover = -1;
                ctx_menu_open = 0;
            } else {
                if (start_menu_open) { start_menu_open = 0; start_menu_hover = -1; }
                ctx_menu_open = 0;
                /* Window buttons in dock */
                int clicked_slot = -1;
                for (int i = 0; i < num_wins; i++) {
                    int bx, by, bw, bh;
                    dock_win_rect(i, &bx, &by, &bw, &bh);
                    if (mouse_x >= bx && mouse_x < bx + bw &&
                        mouse_y >= by && mouse_y < by + bh) {
                        clicked_slot = i;
                        break;
                    }
                }
                if (clicked_slot >= 0) {
                    if (clicked_slot == focus_win && !wins[clicked_slot].minimized) {
                        wins[clicked_slot].minimized = 1;
                        if (drag_win == clicked_slot) { dragging = 0; drag_win = -1; }
                    } else {
                        wins[clicked_slot].minimized = 0;
                        focus_win = clicked_slot;
                    }
                }
            }
        }

        /* ---- Start menu click ---- */
        if (left_click && start_menu_open && mouse_y < tb_y) {
            int item = start_menu_item_at(mouse_x, mouse_y);
            if (item >= 0 && item < NUM_APPS) {
                if (item == 7) ms_init();
                open_window(item);
                start_menu_open = 0; start_menu_hover = -1;
            } else if (item == SM_ITEM_SHUTDOWN) {
                window_running = 0;
            } else if (item == SM_ITEM_RESTART) {
                num_wins = 0;
                focus_win = -1;
                start_menu_open = 0;
                start_menu_hover = -1;
                ctx_menu_open = 0;
                term_clear();
                term_print("SpiritFoxOS Terminal v0.5\n");
                term_print("Type 'help' for commands.\n\n");
                term_input[0] = '\0';
                term_input_len = 0;
            } else if (!point_in_start_menu(mouse_x, mouse_y)) {
                start_menu_open = 0; start_menu_hover = -1;
            }
        }

        /* ---- Minesweeper right-click (flag) ---- */
        if (right_click && mouse_y < tb_y) {
            int handled = 0;
            for (int i = num_wins - 1; i >= 0; i--) {
                if (wins[i].app == 7 && !wins[i].minimized &&
                    mouse_x >= wins[i].x && mouse_x < wins[i].x + wins[i].w &&
                    mouse_y >= wins[i].y && mouse_y < wins[i].y + wins[i].h) {
                    WinSlot *mws = &wins[i];
                    int grid_w = MS_COLS * MS_CELL_SIZE;
                    int grid_h = MS_ROWS * MS_CELL_SIZE;
                    int mgx = mws->x + (mws->w - grid_w) / 2;
                    int mgy = mws->y + TITLE_BAR_HEIGHT + 32;
                    if (mouse_x >= mgx && mouse_x < mgx + grid_w &&
                        mouse_y >= mgy && mouse_y < mgy + grid_h) {
                        int mc = (mouse_x - mgx) / MS_CELL_SIZE;
                        int mr = (mouse_y - mgy) / MS_CELL_SIZE;
                        if (mc >= 0 && mc < MS_COLS && mr >= 0 && mr < MS_ROWS && !ms_game_over)
                            ms_handle_click(mr, mc, 1);
                    }
                    handled = 1;
                    break;
                }
            }
            if (handled) goto skip_desktop_rc;
        }

        /* ---- Desktop right-click ---- */
        if (right_click && mouse_y < tb_y && !point_in_start_menu(mouse_x, mouse_y)) {
            int on_win = 0;
            for (int i = num_wins - 1; i >= 0; i--) {
                if (!wins[i].minimized &&
                    mouse_x >= wins[i].x && mouse_x < wins[i].x + wins[i].w &&
                    mouse_y >= wins[i].y && mouse_y < wins[i].y + wins[i].h) {
                    on_win = 1; break;
                }
            }
            if (!on_win) {
                ctx_menu_open = 1;
                ctx_menu_x = mouse_x;
                ctx_menu_y = mouse_y;
                ctx_menu_hover = -1;
                start_menu_open = 0;
            }
        }
        skip_desktop_rc:

        /* ---- Context menu click ---- */
        if (left_click && ctx_menu_open) {
            int item = ctx_menu_item_at(mouse_x, mouse_y);
            if (item >= 0) {
                switch (item) {
                case 0: open_window(0); break;
                case 1: open_window(1); break;
                case 2: open_window(2); break;
                case 3: open_window(3); break;
                case 5: open_window(4); break;
                case 6: window_running = 0; break;
                }
                ctx_menu_open = 0; ctx_menu_hover = -1;
            } else if (!point_in_ctx_menu(mouse_x, mouse_y)) {
                ctx_menu_open = 0; ctx_menu_hover = -1;
            }
        }

        /* ---- Desktop icon click ---- */
        int click_on_any_window = 0;
        if (left_click && mouse_y < tb_y) {
            if (start_menu_open && !point_in_start_menu(mouse_x, mouse_y)) {
                start_menu_open = 0; start_menu_hover = -1;
            }
            if (ctx_menu_open && !point_in_ctx_menu(mouse_x, mouse_y)) {
                ctx_menu_open = 0; ctx_menu_hover = -1;
            }
            for (int i = num_wins - 1; i >= 0; i--) {
                if (!wins[i].minimized &&
                    mouse_x >= wins[i].x && mouse_x < wins[i].x + wins[i].w &&
                    mouse_y >= wins[i].y && mouse_y < wins[i].y + wins[i].h) {
                    click_on_any_window = 1; break;
                }
            }
        }
        if (left_click && !click_on_any_window && !start_menu_open && !ctx_menu_open && mouse_y < tb_y) {
            for (int i = 0; i < NUM_APPS; i++) {
                int ix, iy;
                icon_grid_pos(i, &ix, &iy);
                if (mouse_x >= ix && mouse_x < ix + ICON_SIZE &&
                    mouse_y >= iy && mouse_y < iy + ICON_SIZE + 18) {
                    icon_pressed = i;
                    if (i == 7) ms_init();
                    open_window(i);
                    break;
                }
            }
        }

        /* ---- Click on windows ---- */
        if (left_click && mouse_y < tb_y && !start_menu_open && !ctx_menu_open) {
            int clicked = -1;

            /* Focused window first */
            if (focus_win >= 0 && !wins[focus_win].minimized) {
                WinSlot *ws = &wins[focus_win];
                if (mouse_x >= ws->x && mouse_x < ws->x + ws->w &&
                    mouse_y >= ws->y && mouse_y < ws->y + ws->h)
                    clicked = focus_win;
            }
            /* Other windows in reverse */
            if (clicked < 0) {
                for (int i = num_wins - 1; i >= 0; i--) {
                    if (i == focus_win || wins[i].minimized) continue;
                    WinSlot *ws = &wins[i];
                    if (mouse_x >= ws->x && mouse_x < ws->x + ws->w &&
                        mouse_y >= ws->y && mouse_y < ws->y + ws->h) {
                        clicked = i; break;
                    }
                }
            }

            if (clicked >= 0 && clicked != focus_win) {
                focus_win = clicked;
                wins[clicked].minimized = 0;
            }

            /* Window button / drag / resize for focused window */
            if (clicked >= 0 && clicked == focus_win) {
                WinSlot *ws = &wins[focus_win];
                int close_x2, min_x2, max_x2, btn_y2;
                win_control_positions(ws->x, ws->y, ws->w, &close_x2, &min_x2, &max_x2, &btn_y2);

                /* macOS-style dot buttons - check if click is in the button area */
                if (mouse_y >= ws->y + 4 && mouse_y < ws->y + TITLE_BAR_HEIGHT - 4 &&
                    mouse_x >= ws->x + 8 && mouse_x < ws->x + 86) {
                    /* Close button */
                    if (mouse_x >= close_x2 && mouse_x < close_x2 + 16) {
                        close_window(focus_win); clicked = -1;
                    }
                    /* Minimize button */
                    else if (mouse_x >= min_x2 && mouse_x < min_x2 + 16) {
                        ws->minimized = 1;
                        dragging = 0; drag_win = -1;
                    }
                    /* Maximize button */
                    else if (mouse_x >= max_x2 && mouse_x < max_x2 + 16) {
                        if (ws->maximized) {
                            ws->x = ws->saved_x; ws->y = ws->saved_y;
                            ws->w = ws->saved_w; ws->h = ws->saved_h;
                            ws->maximized = 0;
                        } else {
                            ws->saved_x = ws->x; ws->saved_y = ws->y;
                            ws->saved_w = ws->w; ws->saved_h = ws->h;
                            ws->x = 0; ws->y = 0;
                            ws->w = (int)fb_get_width();
                            ws->h = dock_y();
                            ws->maximized = 1;
                        }
                        dragging = 0; drag_win = -1;
                    }
                }

                /* Minesweeper cell click */
                if (clicked >= 0 && focus_win >= 0 &&
                    wins[focus_win].app == 7 && !wins[focus_win].minimized) {
                    WinSlot *mws = &wins[focus_win];
                    int grid_w = MS_COLS * MS_CELL_SIZE;
                    int grid_h = MS_ROWS * MS_CELL_SIZE;
                    int mgx = mws->x + (mws->w - grid_w) / 2;
                    int mgy = mws->y + TITLE_BAR_HEIGHT + 32;
                    if (mouse_x >= mgx && mouse_x < mgx + grid_w &&
                        mouse_y >= mgy && mouse_y < mgy + grid_h) {
                        int mc = (mouse_x - mgx) / MS_CELL_SIZE;
                        int mr = (mouse_y - mgy) / MS_CELL_SIZE;
                        if (mc >= 0 && mc < MS_COLS && mr >= 0 && mr < MS_ROWS) {
                            if (ms_game_over) {
                                ms_init();
                            } else {
                                ms_handle_click(mr, mc, 0);
                            }
                        }
                    }
                }

                /* Start dragging if on title bar (not on dot buttons) */
                if (clicked >= 0 && focus_win >= 0 && !wins[focus_win].minimized &&
                    mouse_y >= ws->y && mouse_y < ws->y + TITLE_BAR_HEIGHT &&
                    !(mouse_x >= ws->x + 8 && mouse_x < ws->x + 68 &&
                      mouse_y >= ws->y + 4 && mouse_y < ws->y + TITLE_BAR_HEIGHT - 4)) {

                    /* Double-click on title bar toggles maximize */
                    if (double_click) {
                        if (ws->maximized) {
                            ws->x = ws->saved_x; ws->y = ws->saved_y;
                            ws->w = ws->saved_w; ws->h = ws->saved_h;
                            ws->maximized = 0;
                        } else {
                            ws->saved_x = ws->x; ws->saved_y = ws->y;
                            ws->saved_w = ws->w; ws->saved_h = ws->h;
                            ws->x = 0; ws->y = 0;
                            ws->w = (int)fb_get_width();
                            ws->h = (int)fb_get_height() - TASKBAR_HEIGHT - DOCK_FLOAT_GAP;
                            ws->maximized = 1;
                        }
                        dragging = 0; drag_win = -1;
                    } else if (!wins[focus_win].maximized) {
                        dragging = 1; drag_win = focus_win;
                        drag_off_x = mouse_x - ws->x;
                        drag_off_y = mouse_y - ws->y;
                    }
                }

                /* Start resizing if on edge */
                if (clicked >= 0 && focus_win >= 0 && !wins[focus_win].minimized &&
                    !wins[focus_win].maximized && !dragging) {
                    int edge = get_resize_edge(ws, mouse_x, mouse_y);
                    if (edge) {
                        resizing = 1; resize_win = focus_win; resize_edge = edge;
                        resize_start_x = mouse_x; resize_start_y = mouse_y;
                        resize_orig_w = ws->w; resize_orig_h = ws->h;
                        resize_orig_x = ws->x; resize_orig_y = ws->y;
                    }
                }
            }
        }

        /* ---- Dragging ---- */
        if (dragging && drag_win >= 0) {
            if (left_down && !wins[drag_win].minimized) {
                WinSlot *ws = &wins[drag_win];
                ws->x = mouse_x - drag_off_x;
                ws->y = mouse_y - drag_off_y;
                int sw2 = (int)fb_get_width();
                int sh2 = (int)fb_get_height();
                if (ws->x < 0) ws->x = 0;
                if (ws->y < 0) ws->y = 0;
                if (ws->x + 60 > sw2) ws->x = sw2 - 60;
                if (ws->y + 30 > dock_y()) ws->y = dock_y() - 30;
            } else {
                dragging = 0; drag_win = -1;
            }
        }

        /* ---- Resizing ---- */
        if (resizing && resize_win >= 0) {
            if (left_down && !wins[resize_win].minimized) {
                WinSlot *ws = &wins[resize_win];
                int dx2 = mouse_x - resize_start_x;
                int dy2 = mouse_y - resize_start_y;

                if (resize_edge & 1) {
                    int nw = resize_orig_w + dx2;
                    if (nw >= 160) ws->w = nw;
                }
                if (resize_edge & 2) {
                    int nh = resize_orig_h + dy2;
                    if (nh >= 100) ws->h = nh;
                }
                if (resize_edge & 4) {
                    int nw = resize_orig_w - dx2;
                    if (nw >= 160) { ws->w = nw; ws->x = resize_orig_x + dx2; }
                }
                if (resize_edge & 8) {
                    int nh = resize_orig_h - dy2;
                    if (nh >= 100) { ws->h = nh; ws->y = resize_orig_y + dy2; }
                }
            } else {
                resizing = 0; resize_win = -1;
            }
        }

        /* ---- Draw everything ---- */
        draw_gradient_background();

        /* Desktop icon hover */
        int hover_icon = -1;
        if (mouse_y < tb_y && !start_menu_open && !ctx_menu_open) {
            for (int i = 0; i < NUM_APPS; i++) {
                int ix, iy;
                icon_grid_pos(i, &ix, &iy);
                if (mouse_x >= ix && mouse_x < ix + ICON_SIZE &&
                    mouse_y >= iy && mouse_y < iy + ICON_SIZE + 18) {
                    hover_icon = i; break;
                }
            }
        }
        draw_all_desktop_icons(hover_icon);

        /* Non-focused, non-minimized windows */
        for (int i = 0; i < num_wins; i++) {
            if (i != focus_win && !wins[i].minimized)
                draw_app_window(&wins[i]);
        }
        /* Focused window on top */
        if (focus_win >= 0 && !wins[focus_win].minimized)
            draw_app_window(&wins[focus_win]);

        draw_taskbar();
        if (start_menu_open) draw_start_menu();
        if (ctx_menu_open) draw_ctx_menu();
        draw_mouse_cursor(mouse_x, mouse_y);
        fb_swap_buffer();
        timer_sleep_ms(16);
    }

    restore_vga_text_mode();
}
