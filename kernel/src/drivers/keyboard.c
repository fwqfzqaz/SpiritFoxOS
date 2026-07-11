#include "keyboard.h"
#include "hal.h"

/* 环形缓冲区 */
static char key_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint32_t buf_head;
static volatile uint32_t buf_tail;

/* 键盘状态 */
static int shift_pressed;
static int caps_lock;

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
