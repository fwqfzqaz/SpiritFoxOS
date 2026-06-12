/* SpiritFoxOS - USB核心驱动
 * Copyright (C) 2025 SpiritFoxOS Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "usb.h"
#include "xhci.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "log.h"
#include "../include/io.h"
#include "../include/string.h"

/* ============================================================
 * USB设备注册表
 * ============================================================ */

static usb_device_info_t usb_devices[USB_MAX_DEVICES];
static int usb_device_count = 0;

const char *usb_class_name(uint8_t class_code) {
    switch (class_code) {
        case 0x00: return "Defined at IF";
        case 0x01: return "Audio";
        case 0x02: return "CDC";
        case 0x03: return "HID";
        case 0x05: return "Physical";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case 0x08: return "Mass Storage";
        case 0x09: return "Hub";
        case 0x0A: return "CDC-Data";
        case 0x0B: return "Smart Card";
        case 0x0D: return "Content Sec";
        case 0x0E: return "Video";
        case 0x0F: return "Personal HC";
        case 0x10: return "Audio/Video";
        case 0x11: return "Billboard";
        case 0x12: return "USB Type-C";
        case 0xDC: return "Diagnostic";
        case 0xE0: return "Wireless";
        case 0xEF: return "Misc";
        case 0xFE: return "Application";
        case 0xFF: return "Vendor";
        default:   return "Unknown";
    }
}

const char *usb_speed_name(uint8_t speed) {
    switch (speed) {
        case 1: return "Full-Speed";
        case 2: return "Low-Speed";
        case 3: return "High-Speed";
        case 4: return "Super-Speed";
        default: return "Unknown";
    }
}

/* ============================================================
 * 控制传输辅助函数
 * 封装xhci_control_transfer，针对指定的slot_id。
 * ============================================================ */

static int usb_ctrl(uint8_t slot_id, uint8_t bmRT, uint8_t bReq,
                    uint16_t wVal, uint16_t wIdx, uint16_t wLen, void *buf) {
    xhci_controller_t *ctrl = xhci_get_controller();
    if (!ctrl) return -1;
    return xhci_control_transfer(ctrl, slot_id, bmRT, bReq, wVal, wIdx, wLen, buf);
}

/* ============================================================
 * 描述符读取
 * ============================================================ */

int usb_get_descriptor(uint8_t slot_id, uint8_t desc_type, uint8_t desc_index,
                       void *buf, uint16_t len) {
    memset(buf, 0, len);
    int rc = usb_ctrl(slot_id,
                      0x80, /* 设备到主机 | 标准 | 设备 */
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)((desc_type << 8) | desc_index),
                      0, len, buf);
    return rc;
}

/* ============================================================
 * 配置与接口
 * ============================================================ */

static int usb_set_configuration(uint8_t slot_id, uint8_t config_value) {
    return usb_ctrl(slot_id,
                    0x00, /* 主机到设备 | 标准 | 设备 */
                    USB_REQ_SET_CONFIG,
                    config_value, 0, 0, NULL);
}


/* ============================================================
 * HID启动协议
 * ============================================================ */

int usb_hid_set_boot_protocol(uint8_t slot_id, uint8_t interface_num,
                               uint8_t protocol) {
    /* SET_PROTOCOL请求：
     * bmRequestType = 0x21（主机到设备 | 类 | 接口）
     * bRequest = 0x0B（SET_PROTOCOL）
     * wValue = 0=启动协议, 1=报告协议
     * wIndex = 接口编号 */
    return usb_ctrl(slot_id,
                    0x21, USB_HID_REQ_SET_PROTOCOL,
                    protocol, interface_num, 0, NULL);
}

int usb_hid_get_report(uint8_t slot_id, uint8_t *buf, uint16_t len) {
    (void)slot_id;
    (void)buf;
    (void)len;
    return -1;  /* 未实现 - 使用轮询代替 */
}

/* ============================================================
 * HID设备跟踪（键盘 / 鼠标）
 *
 * 最多跟踪 USB_MAX_HID_DEV 个设备。
 * 每个设备有各自的slot_id、端点信息和状态。
 * ============================================================ */

#define USB_MAX_HID_DEV 8

typedef enum {
    USB_HID_NONE = 0,
    USB_HID_KEYBOARD,
    USB_HID_MOUSE,
} usb_hid_type_t;

typedef struct {
    uint8_t      slot_id;          /* xHCI槽位ID */
    uint8_t      ep_in_addr;       /* 中断IN端点地址 */
    uint8_t      ep_ring_index;    /* 传输环索引（2或3） */
    uint16_t     max_packet;       /* IN端点的最大数据包大小 */
    uint16_t     report_len;       /* 预期的报告长度 */
    usb_hid_type_t type;           /* 键盘或鼠标 */
    int          configured;       /* 端点已配置并就绪？ */
    int          active;           /* 设备仍然存在？ */
} usb_hid_dev_t;

static usb_hid_dev_t hid_devices[USB_MAX_HID_DEV];
static int hid_device_count = 0;

static usb_hid_dev_t *alloc_hid(void) {
    for (int i = 0; i < USB_MAX_HID_DEV; i++) {
        if (!hid_devices[i].active) {
            memset(&hid_devices[i], 0, sizeof(usb_hid_dev_t));
            hid_devices[i].active = 1;
            hid_device_count++;
            return &hid_devices[i];
        }
    }
    return NULL;
}

/* ============================================================
 * 解析配置描述符以查找HID接口和端点。
 * 原始配置描述符包含嵌套的描述符：
 *   [配置描述符] [接口描述符] [HID描述符] [端点描述符] ...
 * 我们遍历它们以找到需要的内容。
 * ============================================================ */

static int parse_config_for_hid(uint8_t slot_id, void *raw_config, uint16_t total_len) {
    uint8_t *p = (uint8_t *)raw_config;
    uint16_t offset = 0;

    while (offset < total_len && offset + 2 <= total_len) {
        uint8_t desc_len = p[offset + 0];
        uint8_t desc_type = p[offset + 1];

        if (desc_len < 2) break;  /* 无效的描述符 */

        if (desc_type == USB_DESC_INTERFACE && desc_len >= 9) {
            /* p+offset处的接口描述符 */
            uint8_t if_class      = p[offset + 5];
            uint8_t if_subclass   = p[offset + 6];
            uint8_t if_protocol   = p[offset + 7];
            uint8_t if_num        = p[offset + 2];
            uint8_t num_eps       = p[offset + 4];

            LOG_I("usb", "  IF%d: class=%02x sub=%02x proto=%02x eps=%d\n",
                   if_num, if_class, if_subclass, if_protocol, num_eps);

            /* 检查HID启动键盘（class=3, subclass=1, proto=1）
             * 或HID启动鼠标（class=3, subclass=1, proto=2） */
            if (if_class == USB_CLASS_HID && if_subclass == 0x01 &&
                (if_protocol == 0x01 || if_protocol == 0x02)) {

                usb_hid_type_t htype = (if_protocol == 1) ? USB_HID_KEYBOARD : USB_HID_MOUSE;
                const char *type_str = (htype == USB_HID_KEYBOARD) ? "Keyboard" : "Mouse";

                /* 在此接口中向前查找中断IN端点 */
                uint16_t ep_offset = offset + desc_len;
                uint8_t eps_found = 0;

                while (eps_found < num_eps && ep_offset + 2 <= total_len) {
                    uint8_t ep_len = p[ep_offset + 0];
                    uint8_t ep_type = p[ep_offset + 1];

                    if (ep_type == USB_DESC_ENDPOINT && ep_len >= 7) {
                        uint8_t  ep_addr    = p[ep_offset + 2];
                        uint8_t  ep_attr    = p[ep_offset + 3];
                        uint16_t ep_max_pkt = p[ep_offset + 4] |
                                             ((uint16_t)p[ep_offset + 5] << 8);
                        uint8_t  ep_interval = p[ep_offset + 6];

                        /* 检查中断IN端点（bit7=IN, 属性位[1:0]=11=中断） */
                        if ((ep_addr & 0x80) && (ep_attr & 0x03) == 0x03) {
                            LOG_I("usb", "  Found %s HID: EP%02X_IN pkt=%d interval=%d\n",
                                   type_str, ep_addr & 0x0F, ep_max_pkt, ep_interval);

                            usb_hid_dev_t *hdev = alloc_hid();
                            if (!hdev) break;

                            hdev->slot_id = slot_id;
                            hdev->ep_in_addr = ep_addr;
                            hdev->max_packet = ep_max_pkt;
                            hdev->type = htype;
                            hdev->report_len = (htype == USB_HID_KEYBOARD) ? 8 : 4;
                            hdev->configured = 0;

                            eps_found++;
                            break;  /* 每个HID设备一个IN端点就足够了 */
                        }
                    }

                    ep_offset += ep_len;
                    if (ep_len == 0) break;
                }
            }
        }

        offset += desc_len;
    }

    return 0;
}

/* ============================================================
 * 通过CONFIG_EP命令配置HID端点
 * 为中断IN端点设置传输环和端点上下文。
 * ============================================================ */

static int configure_hid_endpoint(usb_hid_dev_t *hdev) {
    xhci_controller_t *ctrl = xhci_get_controller();
    if (!ctrl || !ctrl->devices[hdev->slot_id].active) return -1;

    xhci_device_t *dev = &ctrl->devices[hdev->slot_id];
    uint8_t ep_num = hdev->ep_in_addr & 0x0F;

    /* 确定环索引：EP1=index2, EP2=index3, 以此类推 */
    uint8_t ring_idx = ep_num + 1;
    if (ring_idx >= 4) return -1;

    /* 分配输入上下文页 */
    uint64_t ictx_phys = pmm_alloc_pages(1);
    void *ictx = (void *)phys_to_virt(ictx_phys);
    memset(ictx, 0, PAGE_SIZE);

    /* 为此端点初始化传输环 */
    xhci_ring_init(&dev->transfer_rings[ring_idx], XHCI_RING_SIZE);
    uint64_t tr_phys = virt_to_phys((uint64_t)dev->transfer_rings[ring_idx].ring);

    /*
     * 在输入上下文的偏移 64 + (ring_idx-1)*32 处构建端点上下文。
     *
     * xHCI端点上下文布局（32字节 = 8个DWORD）：
     *   DWORD 0: EP状态[1:0] | Mult-1[3:2] | PStreams[7:4] |
     *            Interval[14:8] | LSA[15] | CErr[23:16] | EPType[31:24]
     *   DWORD 1: AvgTRBLen[15:0] | MaxBurst[23:16] | MaxPktSize_hi[31:24]
     *   DWORD 2: TR出队指针低32位
     *   DWORD 3: TR出队指针高32位
     *
     * EPType值：0=无效, 1=同步输出, 2=批量输出, 3=中断输出,
     *          4=控制, 5=同步输入, 6=批量输入, 7=中断输入
     */
    uint8_t *ep_ctx = (uint8_t *)ictx + 64 + (ring_idx - 1) * 32;
    volatile uint32_t *ep_dw = (volatile uint32_t *)ep_ctx;

    /* DWORD 0: 状态=运行中(2), Mult-1=0, PStreams=0,
     *          Interval=0xA（HID约3.2ms）, CErr=3, EPType=中断输入(7) */
    ep_dw[0] = (2u) | (0u << 2) | (0u << 4) | (0x0Au << 8) |
               (0u << 15) | (3u << 16) | (7u << 24);

    /* DWORD 1: AvgTRBLen=最大包长, MaxBurst=0, MaxPktSize_hi=0 */
    ep_dw[1] = (uint32_t)hdev->max_packet;

    /* DWORD 2-3: TR出队指针，DCS=1（周期状态匹配） */
    ep_dw[2] = (uint32_t)(tr_phys & 0xFFFFFFFF);
    ep_dw[3] = (uint32_t)(tr_phys >> 32);

    /* 设置输入控制上下文标志：为我们的端点上下文槽位设置A标志 */
    uint64_t *ictrl = (uint64_t *)ictx;
    ictrl[0] = 0;  /* 无丢弃标志 */
    ictrl[1] = (1u << ring_idx);  /* 为我们的端点上下文设置添加标志 */

    /* 更新设备上下文以指向新的传输环 */
    xhci_dev_ctx_set_ep_ptr(dev->device_ctx, ring_idx, tr_phys);

    /* 发送CONFIG_EP命令 */
    int rc = xhci_configure_endpoint(ctrl, hdev->slot_id, ictx_phys);
    if (rc != 0) {
        LOG_W("usb", "CONFIG_EP failed for HID EP on slot %d (rc=%d)\n",
              hdev->slot_id, rc);
        return rc;
    }

    hdev->ep_ring_index = ring_idx;
    hdev->configured = 1;

    LOG_I("usb", "HID endpoint configured: slot=%d ep_idx=%d type=%s\n",
           hdev->slot_id, ring_idx,
           (hdev->type == USB_HID_KEYBOARD) ? "KB" : "MS");
    return 0;
}

/* ============================================================
 * 完整的HID设备初始化序列：
 * 1. 读取设备描述符
 * 2. 读取完整配置描述符
 * 3. 设置配置
 * 4. 查找HID接口 + 中断IN端点
 * 5. 设置启动协议
 * 6. 配置端点（传输环）
 * ============================================================ */

static int init_single_hid_device(xhci_device_t *xdev) {
    uint8_t slot_id = xdev->slot_id;

    /* 步骤1：读取设备描述符 */
    usb_device_desc_t dev_desc;
    memset(&dev_desc, 0, sizeof(dev_desc));
    int rc = usb_get_descriptor(slot_id, USB_DESC_DEVICE, 0,
                                &dev_desc, sizeof(dev_desc));
    if (rc <= 0) {
        LOG_W("usb", "Slot %d: cannot read device descriptor (rc=%d)\n", slot_id, rc);
        return -1;
    }

    LOG_I("usb", "Slot %d: VID=%04x PID=%04x class=%02x sub=%02x proto=%02x\n",
           slot_id, dev_desc.idVendor, dev_desc.idProduct,
           dev_desc.bDeviceClass, dev_desc.bDeviceSubClass, dev_desc.bDeviceProtocol);

    /* 步骤2：读取第一个配置描述符（仅头部以获取wTotalLength） */
    uint8_t cfg_hdr[9];  /* 最小配置描述符大小 */
    rc = usb_get_descriptor(slot_id, USB_DESC_CONFIG, 0, cfg_hdr, sizeof(cfg_hdr));
    if (rc <= 0) {
        LOG_W("usb", "Slot %d: cannot read config descriptor header\n", slot_id);
        return -1;
    }

    uint16_t total_cfg_len = cfg_hdr[2] | ((uint16_t)cfg_hdr[3] << 8);
    if (total_cfg_len > 512) total_cfg_len = 512;  /* 安全限制 */

    /* 步骤2b：读取完整配置描述符 */
    uint64_t full_cfg_phys = pmm_alloc_pages(1);
    uint8_t *full_cfg = (uint8_t *)phys_to_virt(full_cfg_phys);
    memset(full_cfg, 0, PAGE_SIZE);
    rc = usb_get_descriptor(slot_id, USB_DESC_CONFIG, 0, full_cfg, total_cfg_len);
    if (rc <= 0) {
        LOG_W("usb", "Slot %d: cannot read full config descriptor\n", slot_id);
        pmm_free_page(full_cfg_phys);
        return -1;
    }

    /* 步骤3：设置配置（使用第一个配置的值） */
    uint8_t config_val = cfg_hdr[5];  /* bConfigurationValue */
    rc = usb_set_configuration(slot_id, config_val);
    if (rc < 0) {
        LOG_W("usb", "Slot %d: set configuration failed (rc=%d)\n", slot_id, rc);
        pmm_free_page(full_cfg_phys);
        return -1;
    }

    /* 设置配置后短暂延迟 */
    for (volatile int d = 0; d < 100000; d++);

    /* 步骤4：解析配置以查找HID接口/端点 */
    parse_config_for_hid(slot_id, full_cfg, total_cfg_len);

    /* 步骤5+6：对每个找到的HID设备，设置协议并配置端点 */
    for (int i = 0; i < USB_MAX_HID_DEV; i++) {
        usb_hid_dev_t *hd = &hid_devices[i];
        if (!hd->active || hd->slot_id != slot_id || hd->configured) continue;

        /* 设置HID启动协议 */
        uint8_t boot_proto = (hd->type == USB_HID_KEYBOARD) ? 1 : 0;  /* 键盘=启动协议, 鼠标=启动协议(0) */
        rc = usb_hid_set_boot_protocol(slot_id, 0, boot_proto);
        if (rc < 0) {
            LOG_W("usb", "Slot %d: set boot protocol failed (rc=%d)\n", slot_id, rc);
            hd->active = 0;
            hid_device_count--;
            continue;
        }

        /* 配置中断IN端点 */
        rc = configure_hid_endpoint(hd);
        if (rc < 0) {
            LOG_W("usb", "Slot %d: configure HID endpoint failed (rc=%d)\n", slot_id, rc);
            hd->active = 0;
            hid_device_count--;
            continue;
        }

        const char *tname = (hd->type == USB_HID_KEYBOARD) ? "USB-KB" : "USB-MS";
        LOG_I("usb", "%s initialized: slot=%d EP%d maxpkt=%d\n",
               tname, slot_id, hd->ep_in_addr & 0x0F, hd->max_packet);
    }

    pmm_free_page(full_cfg_phys);
    return 0;
}

/* ============================================================
 * USB HID轮询函数
 *
 * 从gui_run()主循环调用以检查USB输入数据。
 * 这些使用同步中断IN传输（轮询模式）。
 * ============================================================ */

/* 轮询USB HID键盘设备。
 * 读取8字节启动报告并将按键码推入kb_buffer。
 * 报告格式：[修饰键(1)] [保留(1)] [按键码0..按键码5(6)]
 * 返回处理的按键数，或负数错误。 */
int usb_hid_poll_keyboard(usb_hid_dev_t *hdev) {
    if (!hdev || !hdev->active || !hdev->configured ||
        hdev->type != USB_HID_KEYBOARD) return -1;

    static uint8_t prev_keys[6] = {0};
    uint8_t report[8];
    memset(report, 0, sizeof(report));

    xhci_controller_t *ctrl = xhci_get_controller();
    if (!ctrl) return -1;

    int rc = xhci_transfer_data(ctrl, hdev->slot_id, hdev->ep_ring_index,
                                 report, hdev->report_len, 1);  /* direction_in=1 */
    if (rc <= 0) return rc;  /* 无数据或错误 */

    /* 仅在获得完整报告时处理 */
    if (rc < 8) return 0;

    /* 从报告字节[2..7]中提取按键码 */
    uint8_t modifiers = report[0];
    int keys_pushed = 0;

    /* 通过与之前的按键比较来检测按键释放 */
    for (int i = 0; i < 6; i++) {
        int released = 1;
        for (int j = 0; j < 6; j++) {
            if (prev_keys[i] == report[j]) { released = 0; break; }
        }
        /* 按键释放处理可以在这里添加（如需要） */
        (void)released;
    }

    /* 处理新按下的按键 */
    for (int i = 0; i < 6; i++) {
        if (report[2 + i] == 0) continue;  /* 此位置无按键 */
        if (report[2 + i] >= 0x04 && report[2 + i] <= 0x39) {
            /* 有效的键盘HID使用码 - 推入缓冲区 */
            extern void usb_kb_push(uint8_t hid_keycode, uint8_t modifiers);
            usb_kb_push(report[2 + i], modifiers);
            keys_pushed++;
        }
    }

    /* 保存当前按键以供下次比较 */
    for (int i = 0; i < 6; i++)
        prev_keys[i] = report[2 + i];

    return keys_pushed;
}

/* 轮询USB HID鼠标设备。
 * 读取鼠标报告并更新鼠标状态。
 * 启动鼠标报告格式（典型）：
 *   [按钮(1)] [dx(1)] [dy(1)] [可选滚轮(1)]
 * 成功返回0，错误返回负数。 */
int usb_hid_poll_mouse(usb_hid_dev_t *hdev) {
    if (!hdev || !hdev->active || !hdev->configured ||
        hdev->type != USB_HID_MOUSE) return -1;

    uint8_t report[8];
    memset(report, 0, sizeof(report));

    xhci_controller_t *ctrl = xhci_get_controller();
    if (!ctrl) return -1;

    int rc = xhci_transfer_data(ctrl, hdev->slot_id, hdev->ep_ring_index,
                                 report, hdev->report_len, 1);  /* direction_in=1 */
    if (rc <= 0) return rc;  /* 无数据或错误 */

    if (rc < 3) return 0;  /* 至少需要按钮+dx+dy */

    /*
     * 完整性检查：真实的鼠标报告应该有合理的值。
     * 如果所有字节都为零或报告长度错误，这可能是
     * 来自未连接设备的过时/垃圾数据——忽略它。
     */
    int all_zero = 1;
    for (int i = 0; i < rc; i++) {
        if (report[i] != 0) { all_zero = 0; break; }
    }
    if (all_zero) return 0;  /* 全零报告 = 无真实移动 */

    /* 将鼠标移动应用到全局状态 */
    extern void usb_mouse_update(int8_t dx, int8_t dy, uint8_t buttons);

    int8_t dx = (int8_t)report[1];
    int8_t dy = (int8_t)report[2];
    uint8_t btn = report[0];

    usb_mouse_update(dx, dy, btn);
    return 0;
}

/* 轮询所有活动的USB HID设备 */
void usb_hid_poll_all(void) {
    for (int i = 0; i < USB_MAX_HID_DEV; i++) {
        if (!hid_devices[i].active || !hid_devices[i].configured) continue;
        if (hid_devices[i].type == USB_HID_KEYBOARD)
            usb_hid_poll_keyboard(&hid_devices[i]);
        else if (hid_devices[i].type == USB_HID_MOUSE)
            usb_hid_poll_mouse(&hid_devices[i]);
    }
}

/* 检查是否存在USB HID键盘 */
int usb_has_keyboard(void) {
    for (int i = 0; i < USB_MAX_HID_DEV; i++) {
        if (hid_devices[i].active && hid_devices[i].configured &&
            hid_devices[i].type == USB_HID_KEYBOARD) return 1;
    }
    return 0;
}

/* 检查是否存在USB HID鼠标 */
int usb_has_mouse(void) {
    for (int i = 0; i < USB_MAX_HID_DEV; i++) {
        if (hid_devices[i].active && hid_devices[i].configured &&
            hid_devices[i].type == USB_HID_MOUSE) return 1;
    }
    return 0;
}

/* ============================================================
 * 公共API
 * ============================================================ */

void usb_init(void) {
    usb_device_count = 0;
    hid_device_count = 0;
    memset(usb_devices, 0, sizeof(usb_devices));
    memset(hid_devices, 0, sizeof(hid_devices));

    xhci_controller_t *ctrl = xhci_get_controller();
    if (!ctrl) {
        LOG_W("usb", "No xHCI controller available\n");
        return;
    }

    /* 枚举所有设备并尝试初始化HID设备 */
    for (int i = 1; i <= XHCI_MAX_SLOTS; i++) {
        if (ctrl->devices[i].active) {
            /* 注册到通用设备列表（带边界检查） */
            if (usb_device_count >= USB_MAX_DEVICES) {
                LOG_W("usb", "Too many USB devices, skipping slot %d\n", i);
                continue;
            }
            usb_device_info_t *udev = &usb_devices[usb_device_count];
            memset(udev, 0, sizeof(usb_device_info_t));

            udev->slot_id = ctrl->devices[i].slot_id;
            udev->speed = ctrl->devices[i].speed;
            udev->active = 1;

            /* 尝试读取设备描述符 */
            usb_device_desc_t dd;
            if (usb_get_descriptor(i, USB_DESC_DEVICE, 0, &dd, sizeof(dd)) > 0) {
                udev->class_code = dd.bDeviceClass;
                udev->subclass = dd.bDeviceSubClass;
                udev->protocol = dd.bDeviceProtocol;
                udev->vendor_id = dd.idVendor;
                udev->product_id = dd.idProduct;
                udev->num_configs = dd.bNumConfigurations;
            }

            if (udev->class_code == USB_CLASS_HID || udev->class_code == 0x00) {
                udev->hid_type = (udev->protocol == 1) ? 1 :
                                  (udev->protocol == 2) ? 2 : 0;
            }

            LOG_I("usb", "Device slot=%d class=%s VID=%x PID=%x speed=%s\n",
                   udev->slot_id, usb_class_name(udev->class_code),
                   udev->vendor_id, udev->product_id, usb_speed_name(udev->speed));

            usb_device_count++;

            /* 尝试为此设备进行HID初始化 */
            init_single_hid_device(&ctrl->devices[i]);
        }
    }

    LOG_I("usb", "%d device(s) enumerated, %d HID device(s) initialized\n",
           usb_device_count, hid_device_count);
}

int usb_get_device_list(usb_device_info_t *list, int max) {
    int count = 0;
    for (int i = 0; i < usb_device_count && count < max; i++) {
        if (usb_devices[i].active) {
            list[count++] = usb_devices[i];
        }
    }
    return count;
}
