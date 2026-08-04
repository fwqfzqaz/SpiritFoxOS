#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

/* ========================================================================
 * 输入子系统 - Input Subsystem
 * Linux evdev 兼容的输入事件定义
 * ======================================================================== */

/* 输入事件类型 */
#define EV_SYN          0x00    /* 同步事件 */
#define EV_KEY          0x01    /* 按键事件 */
#define EV_REL          0x02    /* 相对轴事件 */
#define EV_ABS          0x03    /* 绝对轴事件 */
#define EV_MSC          0x04    /* 杂项事件 */
#define EV_SW           0x05    /* 开关事件 */
#define EV_LED          0x11    /* LED 事件 */
#define EV_SND          0x12    /* 声音事件 */
#define EV_REP          0x14    /* 重复事件 */
#define EV_FF           0x15    /* 力反馈事件 */

/* 同步事件代码 */
#define SYN_REPORT      0       /* 帧同步 */
#define SYN_CONFIG      1       /* 配置同步 */
#define SYN_DROPPED     3       /* 事件丢失 */

/* 常用按键码 (KEY_*) */
#define KEY_RESERVED    0
#define KEY_ESC         1
#define KEY_1           2
#define KEY_2           3
#define KEY_3           4
#define KEY_4           5
#define KEY_5           6
#define KEY_6           7
#define KEY_7           8
#define KEY_8           9
#define KEY_9           10
#define KEY_0           11
#define KEY_MINUS       12
#define KEY_EQUAL       13
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_Q           16
#define KEY_W           17
#define KEY_E           18
#define KEY_R           19
#define KEY_T           20
#define KEY_Y           21
#define KEY_U           22
#define KEY_I           23
#define KEY_O           24
#define KEY_P           25
#define KEY_LEFTBRACE   26
#define KEY_RIGHTBRACE  27
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29
#define KEY_A           30
#define KEY_S           31
#define KEY_D           32
#define KEY_F           33
#define KEY_G           34
#define KEY_H           35
#define KEY_J           36
#define KEY_K           37
#define KEY_L           38
#define KEY_SEMICOLON   39
#define KEY_APOSTROPHE  40
#define KEY_GRAVE       41
#define KEY_LEFTSHIFT   42
#define KEY_BACKSLASH   43
#define KEY_Z           44
#define KEY_X           45
#define KEY_C           46
#define KEY_V           47
#define KEY_B           48
#define KEY_N           49
#define KEY_M           50
#define KEY_COMMA       51
#define KEY_DOT         52
#define KEY_SLASH       53
#define KEY_RIGHTSHIFT  54
#define KEY_KPASTERISK  55
#define KEY_LEFTALT     56
#define KEY_SPACE       57
#define KEY_CAPSLOCK    58
#define KEY_F1          59
#define KEY_F2          60
#define KEY_F3          61
#define KEY_F4          62
#define KEY_F5          63
#define KEY_F6          64
#define KEY_F7          65
#define KEY_F8          66
#define KEY_F9          67
#define KEY_F10         68
#define KEY_NUMLOCK     69
#define KEY_SCROLLLOCK  70
#define KEY_KP7         71
#define KEY_KP8         72
#define KEY_KP9         73
#define KEY_KPMINUS     74
#define KEY_KP4         75
#define KEY_KP5         76
#define KEY_KP6         77
#define KEY_KPPLUS      78
#define KEY_KP1         79
#define KEY_KP2         80
#define KEY_KP3         81
#define KEY_KP0         82
#define KEY_KPDOT       83
#define KEY_ZENKAKU     85
#define KEY_102ND       86
#define KEY_F11         87
#define KEY_F12         88
#define KEY_RO          89
#define KEY_KATAKANA    90
#define KEY_HIRAGANA    91
#define KEY_HENKAN      92
#define KEY_KATAKANAHIRAGANA 93
#define KEY_MUHENKAN    94
#define KEY_KPJPCOMMA   95
#define KEY_KPENTER     96
#define KEY_RIGHTCTRL   97
#define KEY_KPSLASH     98
#define KEY_SYSRQ       99
#define KEY_RIGHTALT    100
#define KEY_LINEFEED    101
#define KEY_HOME        102
#define KEY_UP          103
#define KEY_PAGEUP      104
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_END         107
#define KEY_DOWN        108
#define KEY_PAGEDOWN    109
#define KEY_INSERT      110
#define KEY_DELETE      111
#define KEY_MACRO       112
#define KEY_MUTE        113
#define KEY_VOLUMEDOWN  114
#define KEY_VOLUMEUP    115
#define KEY_POWER       116
#define KEY_KPEQUAL     117
#define KEY_KPPLUSMINUS 118
#define KEY_PAUSE       119
#define KEY_LEFTMETA    125
#define KEY_RIGHTMETA   126
#define KEY_COMPOSE     127

/* 鼠标按钮码 (BTN_*) */
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112
#define BTN_SIDE        0x113
#define BTN_EXTRA       0x114
#define BTN_FORWARD     0x115
#define BTN_BACK        0x116
#define BTN_TASK        0x117

/* 相对轴代码 (REL_*) */
#define REL_X           0x00
#define REL_Y           0x01
#define REL_Z           0x02
#define REL_RX          0x03
#define REL_RY          0x04
#define REL_RZ          0x05
#define REL_HWHEEL      0x06
#define REL_DIAL        0x07
#define REL_WHEEL       0x08
#define REL_MISC        0x09

/* 绝对轴代码 (ABS_*) */
#define ABS_X           0x00
#define ABS_Y           0x01
#define ABS_Z           0x02
#define ABS_RX          0x03
#define ABS_RY          0x04
#define ABS_RZ          0x05
#define ABS_THROTTLE    0x06
#define ABS_RUDDER      0x07
#define ABS_WHEEL       0x08
#define ABS_GAS         0x09
#define ABS_BRAKE       0x0a
#define ABS_HAT0X       0x10
#define ABS_HAT0Y       0x11
#define ABS_PRESSURE    0x18
#define ABS_DISTANCE    0x19
#define ABS_TILT_X      0x1a
#define ABS_TILT_Y      0x1b

/* LED 代码 (LED_*) */
#define LED_NUML        0x00
#define LED_CAPSL       0x01
#define LED_SCROLLL     0x02
#define LED_COMPOSE     0x03

/* evdev 事件结构体 (Linux input_event 兼容) */
typedef struct {
    int64_t  tv_sec;       /* 时间戳 - 秒 */
    int64_t  tv_usec;      /* 时间戳 - 微秒 */
    uint16_t type;         /* 事件类型 */
    uint16_t code;         /* 事件代码 */
    int32_t  value;        /* 事件值 */
} input_event_t;

/* input_id 结构体 */
typedef struct {
    uint16_t bustype;      /* 总线类型 */
    uint16_t vendor;       /* 厂商 ID */
    uint16_t product;      /* 产品 ID */
    uint16_t version;      /* 版本 */
} input_id_t;

/* 绝对轴信息 */
typedef struct {
    int32_t value;         /* 当前值 */
    int32_t minimum;       /* 最小值 */
    int32_t maximum;       /* 最大值 */
    int32_t fuzz;          /* 噪声容差 */
    int32_t flat;          /* 死区 */
    int32_t resolution;    /* 分辨率 */
} input_absinfo_t;

/* evdev ioctl 命令 */
#define EVIOCGVERSION    _IOR('E', 0x01, int)
#define EVIOCGID         _IOR('E', 0x02, input_id_t)
#define EVIOCGNAME(len)  _IOR('E', 0x06, char[len])
#define EVIOCGPHYS(len)  _IOR('E', 0x07, char[len])
#define EVIOCGKEY(len)   _IOR('E', 0x18, char[len])
#define EVIOCGLED(len)   _IOR('E', 0x19, char[len])
#define EVIOCGBIT(ev, len)  _IOC(_IOC_READ, 'E', 0x20 + ev, len)
#define EVIOCGABS(abs)   _IOR('E', 0x40 + abs, input_absinfo_t)
#define EVIOCSABS(abs)   _IOW('E', 0xc0 + abs, input_absinfo_t)
#define EVIOCSKEYCODE    _IOW('E', 0x04, int[2])
#define EVIOCSLED        _IOW('E', 0x0b, int)

/* evdev 版本 */
#define EV_VERSION  0x010001

/* 总线类型 */
#define BUS_PCI      0x01
#define BUS_USB      0x03
#define BUS_I8042    0x11    /* PS/2 */

/* 位图工具宏 */
#define BITS_TO_LONGS(n)  (((n) + 63) / 64)
#define BITS_PER_LONG     64
#define LONG(x)           ((x) / BITS_PER_LONG)
#define OFFSET(x)         ((x) % BITS_PER_LONG)
#define test_bit(bit, array)  ((array[LONG(bit)] >> OFFSET(bit)) & 1)
#define set_bit(bit, array)   (array[LONG(bit)] |= (1UL << OFFSET(bit)))
#define clear_bit(bit, array) (array[LONG(bit)] &= ~(1UL << OFFSET(bit)))

/* 事件类型计数 */
#define EV_CNT    (EV_FF + 1)
#define KEY_CNT   0x200
#define REL_CNT   0x10
#define ABS_CNT   0x40
#define LED_CNT   0x10

/* ========================================================================
 * 输入设备结构体
 * ======================================================================== */

typedef struct input_dev {
    char name[64];              /* 设备名称 */
    char phys[64];              /* 物理路径 */
    input_id_t id;              /* 设备 ID */

    /* 能力位图 */
    uint64_t evbit[BITS_TO_LONGS(EV_CNT)];    /* 支持的事件类型 */
    uint64_t keybit[BITS_TO_LONGS(KEY_CNT)];  /* 支持的按键 */
    uint64_t relbit[BITS_TO_LONGS(REL_CNT)];  /* 支持的相对轴 */
    uint64_t absbit[BITS_TO_LONGS(ABS_CNT)];  /* 支持的绝对轴 */
    uint64_t ledbit[BITS_TO_LONGS(LED_CNT)];  /* 支持的 LED */

    /* 绝对轴信息 */
    input_absinfo_t absinfo[ABS_CNT];

    /* 当前状态 */
    uint64_t key_state[BITS_TO_LONGS(KEY_CNT)];  /* 当前按键状态 */
    uint64_t led_state[BITS_TO_LONGS(LED_CNT)];  /* 当前 LED 状态 */

    /* evdev 接口数据 */
    int evdev_minor;            /* evdev 设备号 (-1 = 未注册) */
    uint32_t chardev_id;        /* 字符设备 ID */
    void *private;              /* 驱动私有数据 */

    /* 设备列表 */
    struct input_dev *next;
} input_dev_t;

/* ========================================================================
 * 输入核心层 API
 * ======================================================================== */

/* 初始化输入子系统 */
void input_init(void);

/* 分配并注册输入设备 */
input_dev_t *input_allocate_device(void);
int input_register_device(input_dev_t *dev);
void input_unregister_device(input_dev_t *dev);

/* 设置设备能力 */
void input_set_capability(input_dev_t *dev, uint16_t type, uint16_t code);

/* 上报输入事件 */
void input_event(input_dev_t *dev, uint16_t type, uint16_t code, int32_t value);
void input_report_key(input_dev_t *dev, uint16_t code, int32_t value);
void input_report_rel(input_dev_t *dev, uint16_t code, int32_t value);
void input_report_abs(input_dev_t *dev, uint16_t code, int32_t value);
void input_sync(input_dev_t *dev);

/* 获取设备列表 */
input_dev_t *input_get_device_list(void);
input_dev_t *input_get_device(int index);

/* LED 控制 */
int input_set_led(input_dev_t *dev, uint16_t led, int32_t value);

/* evdev 初始化 (注册 /dev/input/event* 设备节点) */
void evdev_init(void);

/* evdev 设备注册 (由 input_register_device 调用) */
void evdev_register_device(input_dev_t *dev);

/* evdev 事件分发 (由 input_sync 调用) */
void evdev_dispatch(input_dev_t *dev, input_event_t *events, int count);

#endif /* INPUT_H */
