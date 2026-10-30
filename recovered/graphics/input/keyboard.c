#include "keyboard.h"
#include "hal.h"

/* keyboard.h 与 input.h 都定义了 KEY_UP/KEY_DOWN/KEY_LEFT/KEY_RIGHT/
 * KEY_HOME/KEY_END/KEY_DELETE/KEY_TAB，但语义不同：
 *   - keyboard.h 中为内部扩展键码（0x80 起），用于 ASCII 缓冲区
 *   - input.h 中为 evdev 按键码（Linux 值），用于 evdev 上报
 * 先取消 keyboard.h 的定义，让 input.h 干净地定义 evdev 值供扫描码
 * 映射表使用；映射表之后再恢复 keyboard.h 的值供 keyboard_handler 使用。 */
#undef KEY_TAB
#undef KEY_UP
#undef KEY_DOWN
#undef KEY_LEFT
#undef KEY_RIGHT
#undef KEY_HOME
#undef KEY_END
#undef KEY_DELETE

#include "input.h"
#include "string.h"
#include "vga.h"

/* 环形缓冲区 */
static char key_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint32_t buf_head;
static volatile uint32_t buf_tail;

/* 键盘状态 */
static int shift_pressed;
static int caps_lock;

/* evdev 输入设备 */
static input_dev_t *keyboard_dev = NULL;

/* 扫描码集 1 (make code 0x00-0x7F) -> evdev 按键码 */
static const uint16_t scancode_set1_keycode[128] = {
    0,            /* 0x00 */
    KEY_ESC,      /* 0x01 */
    KEY_1,        /* 0x02 */
    KEY_2,        /* 0x03 */
    KEY_3,        /* 0x04 */
    KEY_4,        /* 0x05 */
    KEY_5,        /* 0x06 */
    KEY_6,        /* 0x07 */
    KEY_7,        /* 0x08 */
    KEY_8,        /* 0x09 */
    KEY_9,        /* 0x0A */
    KEY_0,        /* 0x0B */
    KEY_MINUS,    /* 0x0C */
    KEY_EQUAL,    /* 0x0D */
    KEY_BACKSPACE,/* 0x0E */
    KEY_TAB,      /* 0x0F */
    KEY_Q,        /* 0x10 */
    KEY_W,        /* 0x11 */
    KEY_E,        /* 0x12 */
    KEY_R,        /* 0x13 */
    KEY_T,        /* 0x14 */
    KEY_Y,        /* 0x15 */
    KEY_U,        /* 0x16 */
    KEY_I,        /* 0x17 */
    KEY_O,        /* 0x18 */
    KEY_P,        /* 0x19 */
    KEY_LEFTBRACE,/* 0x1A */
    KEY_RIGHTBRACE,/*0x1B */
    KEY_ENTER,    /* 0x1C */
    KEY_LEFTCTRL, /* 0x1D */
    KEY_A,        /* 0x1E */
    KEY_S,        /* 0x1F */
    KEY_D,        /* 0x20 */
    KEY_F,        /* 0x21 */
    KEY_G,        /* 0x22 */
    KEY_H,        /* 0x23 */
    KEY_J,        /* 0x24 */
    KEY_K,        /* 0x25 */
    KEY_L,        /* 0x26 */
    KEY_SEMICOLON,/* 0x27 */
    KEY_APOSTROPHE,/*0x28 */
    KEY_GRAVE,    /* 0x29 */
    KEY_LEFTSHIFT,/* 0x2A */
    KEY_BACKSLASH,/* 0x2B */
    KEY_Z,        /* 0x2C */
    KEY_X,        /* 0x2D */
    KEY_C,        /* 0x2E */
    KEY_V,        /* 0x2F */
    KEY_B,        /* 0x30 */
    KEY_N,        /* 0x31 */
    KEY_M,        /* 0x32 */
    KEY_COMMA,    /* 0x33 */
    KEY_DOT,      /* 0x34 */
    KEY_SLASH,    /* 0x35 */
    KEY_RIGHTSHIFT,/*0x36 */
    KEY_KPASTERISK,/*0x37 */
    KEY_LEFTALT,  /* 0x38 */
    KEY_SPACE,    /* 0x39 */
    KEY_CAPSLOCK, /* 0x3A */
    KEY_F1,       /* 0x3B */
    KEY_F2,       /* 0x3C */
    KEY_F3,       /* 0x3D */
    KEY_F4,       /* 0x3E */
    KEY_F5,       /* 0x3F */
    KEY_F6,       /* 0x40 */
    KEY_F7,       /* 0x41 */
    KEY_F8,       /* 0x42 */
    KEY_F9,       /* 0x43 */
    KEY_F10,      /* 0x44 */
    KEY_NUMLOCK,  /* 0x45 */
    KEY_SCROLLLOCK,/*0x46 */
    KEY_KP7,      /* 0x47 */
    KEY_KP8,      /* 0x48 */
    KEY_KP9,      /* 0x49 */
    KEY_KPMINUS,  /* 0x4A */
    KEY_KP4,      /* 0x4B */
    KEY_KP5,      /* 0x4C */
    KEY_KP6,      /* 0x4D */
    KEY_KPPLUS,   /* 0x4E */
    KEY_KP1,      /* 0x4F */
    KEY_KP2,      /* 0x50 */
    KEY_KP3,      /* 0x51 */
    KEY_KP0,      /* 0x52 */
    KEY_KPDOT,    /* 0x53 */
    0, 0, 0,      /* 0x54-0x56 */
    KEY_F11,      /* 0x57 */
    KEY_F12,      /* 0x58 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0x59-0x62 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0x63-0x6C */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0x6D-0x76 */
    0, 0, 0, 0, 0, 0, 0, 0, 0      /* 0x77-0x7F */
};

/* E0 前缀扩展扫描码 (make code 0x00-0x7F) -> evdev 按键码 */
static const uint16_t scancode_set1_e0_keycode[128] = {
    /* 0x00-0x0F */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x10-0x1F */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KEY_KPENTER, KEY_RIGHTCTRL, 0, 0,
    /* 0x20-0x2F */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x30-0x3F */ 0, 0, 0, 0, 0, KEY_KPSLASH, 0, 0, KEY_RIGHTALT, 0, 0, 0, 0, 0, 0, 0,
    /* 0x40-0x4F */ 0, 0, 0, 0, 0, 0, 0, KEY_HOME, KEY_UP, KEY_PAGEUP, 0, KEY_LEFT, 0, KEY_RIGHT, 0, KEY_END,
    /* 0x50-0x5F */ KEY_DOWN, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE, 0, 0, 0, 0, 0, 0, 0, KEY_LEFTMETA, KEY_RIGHTMETA, KEY_COMPOSE, 0, 0,
    /* 0x60-0x6F */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x70-0x7F */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/* 恢复 keyboard.h 的内部扩展键码值，供 keyboard_handler 使用 */
#undef KEY_TAB
#define KEY_TAB    '\t'
#undef KEY_UP
#define KEY_UP     0x80
#undef KEY_DOWN
#define KEY_DOWN   0x81
#undef KEY_LEFT
#define KEY_LEFT   0x82
#undef KEY_RIGHT
#define KEY_RIGHT  0x83
#undef KEY_HOME
#define KEY_HOME   0x84
#undef KEY_END
#define KEY_END    0x85
#undef KEY_DELETE
#define KEY_DELETE 0x86

/* 美式键盘布局扫描码集 1 - 未按 Shift */
static const char scancode_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

/* 美式键盘布局扫描码集 1 - 已按 Shift */
static const char scancode_to_ascii_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static void buffer_put(char c)
{
    uint32_t next = (buf_head + 1) % KEYBOARD_BUFFER_SIZE;
    if (next != buf_tail) {
        key_buffer[buf_head] = c;
        buf_head = next;
    }
}

static char buffer_get(void)
{
    if (buf_head == buf_tail)
        return 0;
    char c = key_buffer[buf_tail];
    buf_tail = (buf_tail + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}

static int buffer_empty(void)
{
    return buf_head == buf_tail;
}

void keyboard_handler(void)
{
    uint8_t scancode = hal_inb(0x60);

    /* 注意：EOI 现在由 idt.c 中的中央 irq_handler() 发送，
     * 该函数调用 apic_eoi()。我们不再在此处发送 PIC EOI。 */

    /* 扩展扫描码前缀 */
    static int e0_prefix = 0;

    if (scancode == 0xE0) {
        e0_prefix = 1;
        return;
    }

    /* ---- evdev 事件上报 ---- */
    if (keyboard_dev) {
        uint8_t  make  = scancode & 0x7F;
        int      value = (scancode & 0x80) ? 0 : 1;
        uint16_t evcode = e0_prefix ? scancode_set1_e0_keycode[make]
                                    : scancode_set1_keycode[make];
        if (evcode) {
            input_report_key(keyboard_dev, evcode, value);
            input_sync(keyboard_dev);
        }
    }
    /* ---- evdev 事件上报结束 ---- */

    /* 处理扩展（E0 前缀）扫描码 */
    if (e0_prefix) {
        e0_prefix = 0;

        /* 扩展键释放：高位置位 */
        if (scancode & 0x80) {
            /* 忽略扩展键释放 */
            return;
        }

        /* 扩展键按下 */
        switch (scancode) {
        case 0x48: buffer_put(KEY_UP);     return;
        case 0x50: buffer_put(KEY_DOWN);   return;
        case 0x4B: buffer_put(KEY_LEFT);   return;
        case 0x4D: buffer_put(KEY_RIGHT);  return;
        case 0x47: buffer_put(KEY_HOME);   return;
        case 0x4F: buffer_put(KEY_END);    return;
        case 0x53: buffer_put(KEY_DELETE); return;
        case 0x49: buffer_put(KEY_PGUP);   return;
        case 0x51: buffer_put(KEY_PGDN);   return;
        default: return;
        }
    }

    /* 按键释放：高位置位 */
    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) {
            shift_pressed = 0;
        }
        return;
    }

    /* 按键按下 */
    switch (scancode) {
    case 0x2A: /* 左 Shift */
    case 0x36: /* 右 Shift */
        shift_pressed = 1;
        return;
    case 0x3A: /* 大写锁定 */
        caps_lock = !caps_lock;
        return;
    default:
        break;
    }

    if (scancode >= 128) {
        return;
    }

    char c;
    if (shift_pressed) {
        c = scancode_to_ascii_shift[scancode];
    } else {
        c = scancode_to_ascii[scancode];
    }

    /* 对字母应用大写锁定 */
    if (caps_lock && c >= 'a' && c <= 'z') {
        c -= 32;
    } else if (caps_lock && c >= 'A' && c <= 'Z') {
        c += 32;
    }

    if (c) {
        buffer_put(c);
    }
}

void keyboard_init(void)
{
    buf_head = 0;
    buf_tail = 0;
    shift_pressed = 0;
    caps_lock = 0;

    /* 键盘 IRQ 现在由 apic_init() 通过 IOAPIC 路由。
     * 不再需要在 PIC 上使能 IRQ1。 */
}

char keyboard_get_char(void)
{
    while (buffer_empty()) {
        /* 轮询 PS/2 控制器：如果键盘有数据但 IRQ 未触发，
         * 则手动读取。这处理 IOAPIC/LAPIC 未能
         * 传递中断的情况（例如 UEFI 后的陈旧 IRQ 线）。 */
        if (hal_inb(0x64) & 0x01) {
            keyboard_handler();
        }
        __asm__ volatile ("hlt");
    }
    return buffer_get();
}

int keyboard_has_char(void)
{
    return !buffer_empty();
}

/* ========================================================================
 * evdev 输入设备初始化
 * ======================================================================== */

void keyboard_input_init(void)
{
    keyboard_dev = input_allocate_device();
    if (!keyboard_dev) {
        printf("[KEYBOARD] failed to allocate input device\n");
        return;
    }

    strncpy(keyboard_dev->name, "SpiritFoxOS Keyboard", sizeof(keyboard_dev->name) - 1);
    strncpy(keyboard_dev->phys, "isa0060/serio0/input0", sizeof(keyboard_dev->phys) - 1);
    keyboard_dev->id.bustype = BUS_I8042;
    keyboard_dev->id.vendor  = 1;
    keyboard_dev->id.product = 1;
    keyboard_dev->id.version = 1;

    /* 声明支持的事件类型与按键码 */
    for (int i = 0; i < 128; i++) {
        if (scancode_set1_keycode[i])
            input_set_capability(keyboard_dev, EV_KEY, scancode_set1_keycode[i]);
        if (scancode_set1_e0_keycode[i])
            input_set_capability(keyboard_dev, EV_KEY, scancode_set1_e0_keycode[i]);
    }

    /* LED 与重复事件 */
    input_set_capability(keyboard_dev, EV_LED, LED_NUML);
    input_set_capability(keyboard_dev, EV_LED, LED_CAPSL);
    input_set_capability(keyboard_dev, EV_LED, LED_SCROLLL);
    input_set_capability(keyboard_dev, EV_REP, 0);

    input_register_device(keyboard_dev);
}

input_dev_t *keyboard_get_input_dev(void)
{
    return keyboard_dev;
}
