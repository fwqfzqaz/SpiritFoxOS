#include "keyboard.h"
#include "hal.h"

/* Circular buffer */
static char key_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint32_t buf_head;
static volatile uint32_t buf_tail;

/* Keyboard state */
static int shift_pressed;
static int caps_lock;

/* US keyboard layout scancode set 1 - unshifted */
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

/* US keyboard layout scancode set 1 - shifted */
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

    /* NOTE: EOI is now sent by the central irq_handler() in idt.c,
     * which calls apic_eoi(). We no longer send PIC EOI here. */

    /* Extended scancode prefix */
    static int e0_prefix = 0;

    if (scancode == 0xE0) {
        e0_prefix = 1;
        return;
    }

    /* Handle extended (E0-prefixed) scancodes */
    if (e0_prefix) {
        e0_prefix = 0;

        /* Extended key release: high bit set */
        if (scancode & 0x80) {
            /* Just ignore extended key releases */
            return;
        }

        /* Extended key press */
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

    /* Key release: high bit set */
    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) {
            shift_pressed = 0;
        }
        return;
    }

    /* Key press */
    switch (scancode) {
    case 0x2A: /* Left shift */
    case 0x36: /* Right shift */
        shift_pressed = 1;
        return;
    case 0x3A: /* Caps lock */
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

    /* Apply caps lock to letters */
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

    /* Keyboard IRQ is now routed through IOAPIC by apic_init().
     * No need to enable IRQ1 on PIC anymore. */
}

char keyboard_get_char(void)
{
    while (buffer_empty()) {
        __asm__ volatile ("hlt");
    }
    return buffer_get();
}

int keyboard_has_char(void)
{
    return !buffer_empty();
}
