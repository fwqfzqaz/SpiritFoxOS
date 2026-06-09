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
 * USB Device Registry
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
 * Control Transfer Helper
 * Wraps xhci_control_transfer for a given slot_id.
 * ============================================================ */

static int usb_ctrl(uint8_t slot_id, uint8_t bmRT, uint8_t bReq,
                    uint16_t wVal, uint16_t wIdx, uint16_t wLen, void *buf) {
    xhci_controller_t *ctrl = xhci_get_controller();
    if (!ctrl) return -1;
    return xhci_control_transfer(ctrl, slot_id, bmRT, bReq, wVal, wIdx, wLen, buf);
}

/* ============================================================
 * Descriptor Reading
 * ============================================================ */

int usb_get_descriptor(uint8_t slot_id, uint8_t desc_type, uint8_t desc_index,
                       void *buf, uint16_t len) {
    memset(buf, 0, len);
    int rc = usb_ctrl(slot_id,
                      0x80, /* device-to-host | standard | device */
                      USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)((desc_type << 8) | desc_index),
                      0, len, buf);
    return rc;
}

/* ============================================================
 * Configuration & Interface
 * ============================================================ */

static int usb_set_configuration(uint8_t slot_id, uint8_t config_value) {
    return usb_ctrl(slot_id,
                    0x00, /* host-to-device | standard | device */
                    USB_REQ_SET_CONFIG,
                    config_value, 0, 0, NULL);
}


/* ============================================================
 * HID Boot Protocol
 * ============================================================ */

int usb_hid_set_boot_protocol(uint8_t slot_id, uint8_t interface_num,
                               uint8_t protocol) {
    /* SET_PROTOCOL request:
     * bmRequestType = 0x21 (host-to-device | class | interface)
     * bRequest = 0x0B (SET_PROTOCOL)
     * wValue = 0=Boot, 1=Report
     * wIndex = interface number */
    return usb_ctrl(slot_id,
                    0x21, USB_HID_REQ_SET_PROTOCOL,
                    protocol, interface_num, 0, NULL);
}

int usb_hid_get_report(uint8_t slot_id, uint8_t *buf, uint16_t len) {
    (void)slot_id;
    (void)buf;
    (void)len;
    return -1;  /* Not implemented - use polling instead */
}

/* ============================================================
 * HID Device Tracking (keyboard / mouse)
 *
 * We track up to USB_MAX_HID_DEV devices.
 * Each has its slot_id, endpoint info, and state.
 * ============================================================ */

#define USB_MAX_HID_DEV 8

typedef enum {
    USB_HID_NONE = 0,
    USB_HID_KEYBOARD,
    USB_HID_MOUSE,
} usb_hid_type_t;

typedef struct {
    uint8_t      slot_id;          /* xHCI slot ID */
    uint8_t      ep_in_addr;       /* Interrupt IN endpoint address */
    uint8_t      ep_ring_index;    /* Transfer ring index (2 or 3) */
    uint16_t     max_packet;       /* Max packet size for IN EP */
    uint16_t     report_len;       /* Expected report length */
    usb_hid_type_t type;           /* KEYBOARD or MOUSE */
    int          configured;       /* Endpoint configured and ready? */
    int          active;           /* Device still present? */
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
 * Parse configuration descriptor to find HID interfaces and endpoints.
 * The raw config descriptor contains nested descriptors:
 *   [config_desc] [interface_desc] [hid_desc] [endpoint_desc] ...
 * We walk through them to find what we need.
 * ============================================================ */

static int parse_config_for_hid(uint8_t slot_id, void *raw_config, uint16_t total_len) {
    uint8_t *p = (uint8_t *)raw_config;
    uint16_t offset = 0;

    while (offset < total_len && offset + 2 <= total_len) {
        uint8_t desc_len = p[offset + 0];
        uint8_t desc_type = p[offset + 1];

        if (desc_len < 2) break;  /* Invalid descriptor */

        if (desc_type == USB_DESC_INTERFACE && desc_len >= 9) {
            /* Interface Descriptor at p+offset */
            uint8_t if_class      = p[offset + 5];
            uint8_t if_subclass   = p[offset + 6];
            uint8_t if_protocol   = p[offset + 7];
            uint8_t if_num        = p[offset + 2];
            uint8_t num_eps       = p[offset + 4];

            LOG_I("usb", "  IF%d: class=%02x sub=%02x proto=%02x eps=%d\n",
                   if_num, if_class, if_subclass, if_protocol, num_eps);

            /* Check for HID Boot Keyboard (class=3, subclass=1, proto=1)
             * or HID Boot Mouse (class=3, subclass=1, proto=2) */
            if (if_class == USB_CLASS_HID && if_subclass == 0x01 &&
                (if_protocol == 0x01 || if_protocol == 0x02)) {

                usb_hid_type_t htype = (if_protocol == 1) ? USB_HID_KEYBOARD : USB_HID_MOUSE;
                const char *type_str = (htype == USB_HID_KEYBOARD) ? "Keyboard" : "Mouse";

                /* Look ahead for an interrupt IN endpoint in this interface */
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

                        /* Check for Interrupt IN (bit7=IN, attr bits[1:0]=11=interrupt) */
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
                            break;  /* One IN EP per HID device is enough */
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
 * Configure HID endpoint via CONFIG_EP command
 * Sets up transfer ring and endpoint context for the interrupt IN EP.
 * ============================================================ */

static int configure_hid_endpoint(usb_hid_dev_t *hdev) {
    xhci_controller_t *ctrl = xhci_get_controller();
    if (!ctrl || !ctrl->devices[hdev->slot_id].active) return -1;

    xhci_device_t *dev = &ctrl->devices[hdev->slot_id];
    uint8_t ep_num = hdev->ep_in_addr & 0x0F;

    /* Determine ring index: EP1=index2, EP2=index3, etc. */
    uint8_t ring_idx = ep_num + 1;
    if (ring_idx >= 4) return -1;

    /* Allocate input context page */
    uint64_t ictx_phys = pmm_alloc_pages(1);
    void *ictx = (void *)phys_to_virt(ictx_phys);
    memset(ictx, 0, PAGE_SIZE);

    /* Initialize transfer ring for this endpoint */
    xhci_ring_init(&dev->transfer_rings[ring_idx], XHCI_RING_SIZE);
    uint64_t tr_phys = virt_to_phys((uint64_t)dev->transfer_rings[ring_idx].ring);

    /*
     * Build Endpoint Context at offset 64 + (ring_idx-1)*32 in input context.
     *
     * xHCI Endpoint Context layout (32 bytes = 8 DWORDs):
     *   DWORD 0: EP State[1:0] | Mult-1[3:2] | PStreams[7:4] |
     *            Interval[14:8] | LSA[15] | CErr[23:16] | EPType[31:24]
     *   DWORD 1: AvgTRBLen[15:0] | MaxBurst[23:16] | MaxPktSize_hi[31:24]
     *   DWORD 2: TR Dequeue Lo
     *   DWORD 3: TR Dequeue Hi
     *
     * EPType values: 0=Invalid, 1=IsochOut, 2=BulkOut, 3=IntOut,
     *               4=Control, 5=IsochIn, 6=BulkIn, 7=IntIn
     */
    uint8_t *ep_ctx = (uint8_t *)ictx + 64 + (ring_idx - 1) * 32;
    volatile uint32_t *ep_dw = (volatile uint32_t *)ep_ctx;

    /* DWORD 0: State=Running(2), Mult-1=0, PStreams=0,
     *          Interval=0xA (~3.2ms for HID), CErr=3, EPType=IntIn(7) */
    ep_dw[0] = (2u) | (0u << 2) | (0u << 4) | (0x0Au << 8) |
               (0u << 15) | (3u << 16) | (7u << 24);

    /* DWORD 1: AvgTRBLen=max_pkt, MaxBurst=0, MaxPktSize_hi=0 */
    ep_dw[1] = (uint32_t)hdev->max_packet;

    /* DWORD 2-3: TR Dequeue Pointer with DCS=1 (cycle state matches) */
    ep_dw[2] = (uint32_t)(tr_phys & 0xFFFFFFFF);
    ep_dw[3] = (uint32_t)(tr_phys >> 32);

    /* Set input control context flags: A-flag for our EP context slot */
    uint64_t *ictrl = (uint64_t *)ictx;
    ictrl[0] = 0;  /* No drop flags */
    ictrl[1] = (1u << ring_idx);  /* Add flag for our EP context */

    /* Update device context to point to the new transfer ring */
    xhci_dev_ctx_set_ep_ptr(dev->device_ctx, ring_idx, tr_phys);

    /* Send CONFIG_EP command */
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
 * Full HID device initialization sequence:
 * 1. Read device descriptor
 * 2. Read full configuration descriptor
 * 3. Set configuration
 * 4. Find HID interface + interrupt IN endpoint
 * 5. Set boot protocol
 * 6. Configure endpoint (transfer ring)
 * ============================================================ */

static int init_single_hid_device(xhci_device_t *xdev) {
    uint8_t slot_id = xdev->slot_id;

    /* Step 1: Read device descriptor */
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

    /* Step 2: Read first configuration descriptor (just header to get wTotalLength) */
    uint8_t cfg_hdr[9];  /* Minimum config descriptor size */
    rc = usb_get_descriptor(slot_id, USB_DESC_CONFIG, 0, cfg_hdr, sizeof(cfg_hdr));
    if (rc <= 0) {
        LOG_W("usb", "Slot %d: cannot read config descriptor header\n", slot_id);
        return -1;
    }

    uint16_t total_cfg_len = cfg_hdr[2] | ((uint16_t)cfg_hdr[3] << 8);
    if (total_cfg_len > 512) total_cfg_len = 512;  /* Safety limit */

    /* Step 2b: Read full configuration descriptor */
    uint64_t full_cfg_phys = pmm_alloc_pages(1);
    uint8_t *full_cfg = (uint8_t *)phys_to_virt(full_cfg_phys);
    memset(full_cfg, 0, PAGE_SIZE);
    rc = usb_get_descriptor(slot_id, USB_DESC_CONFIG, 0, full_cfg, total_cfg_len);
    if (rc <= 0) {
        LOG_W("usb", "Slot %d: cannot read full config descriptor\n", slot_id);
        pmm_free_page(full_cfg_phys);
        return -1;
    }

    /* Step 3: Set configuration (use first config's value) */
    uint8_t config_val = cfg_hdr[5];  /* bConfigurationValue */
    rc = usb_set_configuration(slot_id, config_val);
    if (rc < 0) {
        LOG_W("usb", "Slot %d: set configuration failed (rc=%d)\n", slot_id, rc);
        pmm_free_page(full_cfg_phys);
        return -1;
    }

    /* Small delay after set configuration */
    for (volatile int d = 0; d < 100000; d++);

    /* Step 4: Parse config to find HID interfaces/endpoints */
    parse_config_for_hid(slot_id, full_cfg, total_cfg_len);

    /* Step 5+6: For each HID device found, set protocol and configure endpoint */
    for (int i = 0; i < USB_MAX_HID_DEV; i++) {
        usb_hid_dev_t *hd = &hid_devices[i];
        if (!hd->active || hd->slot_id != slot_id || hd->configured) continue;

        /* Set HID boot protocol */
        uint8_t boot_proto = (hd->type == USB_HID_KEYBOARD) ? 1 : 0;  /* Keyboard=boot, Mouse=boot(0) */
        rc = usb_hid_set_boot_protocol(slot_id, 0, boot_proto);
        if (rc < 0) {
            LOG_W("usb", "Slot %d: set boot protocol failed (rc=%d)\n", slot_id, rc);
            hd->active = 0;
            hid_device_count--;
            continue;
        }

        /* Configure interrupt IN endpoint */
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
 * USB HID Polling Functions
 *
 * Called from gui_run() main loop to check for USB input data.
 * These use synchronous interrupt-IN transfers (polling mode).
 * ============================================================ */

/* Poll a USB HID keyboard device.
 * Reads an 8-byte boot report and pushes key codes into kb_buffer.
 * Report format: [modifier(1)] [reserved(1)] [keycode0..keycode5(6)]
 * Returns number of keys processed, or negative error. */
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
    if (rc <= 0) return rc;  /* No data or error */

    /* Only process if we got a full report */
    if (rc < 8) return 0;

    /* Extract key codes from report bytes [2..7] */
    uint8_t modifiers = report[0];
    int keys_pushed = 0;

    /* Detect key releases by comparing with previous keys */
    for (int i = 0; i < 6; i++) {
        int released = 1;
        for (int j = 0; j < 6; j++) {
            if (prev_keys[i] == report[j]) { released = 0; break; }
        }
        /* Key release handling could go here if needed */
        (void)released;
    }

    /* Process newly pressed keys */
    for (int i = 0; i < 6; i++) {
        if (report[2 + i] == 0) continue;  /* No key in this slot */
        if (report[2 + i] >= 0x04 && report[2 + i] <= 0x39) {
            /* Valid HID usage code for keyboard - push it */
            extern void usb_kb_push(uint8_t hid_keycode, uint8_t modifiers);
            usb_kb_push(report[2 + i], modifiers);
            keys_pushed++;
        }
    }

    /* Save current keys for next comparison */
    for (int i = 0; i < 6; i++)
        prev_keys[i] = report[2 + i];

    return keys_pushed;
}

/* Poll a USB HID mouse device.
 * Reads mouse report and updates mouse state.
 * Boot mouse report format (typically):
 *   [buttons(1)] [dx(1)] [dy(1)] [optional wheel(1)]
 * Returns 0 on success, negative on error. */
int usb_hid_poll_mouse(usb_hid_dev_t *hdev) {
    if (!hdev || !hdev->active || !hdev->configured ||
        hdev->type != USB_HID_MOUSE) return -1;

    uint8_t report[8];
    memset(report, 0, sizeof(report));

    xhci_controller_t *ctrl = xhci_get_controller();
    if (!ctrl) return -1;

    int rc = xhci_transfer_data(ctrl, hdev->slot_id, hdev->ep_ring_index,
                                 report, hdev->report_len, 1);  /* direction_in=1 */
    if (rc <= 0) return rc;  /* No data or error */

    if (rc < 3) return 0;  /* Need at least buttons + dx + dy */

    /*
     * Sanity check: a real mouse report should have reasonable values.
     * If all bytes are zero or the report length is wrong, it's likely
     * stale/garbage data from an unconnected device — ignore it.
     */
    int all_zero = 1;
    for (int i = 0; i < rc; i++) {
        if (report[i] != 0) { all_zero = 0; break; }
    }
    if (all_zero) return 0;  /* All-zero report = no real movement */

    /* Apply mouse movement to global state */
    extern void usb_mouse_update(int8_t dx, int8_t dy, uint8_t buttons);

    int8_t dx = (int8_t)report[1];
    int8_t dy = (int8_t)report[2];
    uint8_t btn = report[0];

    usb_mouse_update(dx, dy, btn);
    return 0;
}

/* Poll all active USB HID devices */
void usb_hid_poll_all(void) {
    for (int i = 0; i < USB_MAX_HID_DEV; i++) {
        if (!hid_devices[i].active || !hid_devices[i].configured) continue;
        if (hid_devices[i].type == USB_HID_KEYBOARD)
            usb_hid_poll_keyboard(&hid_devices[i]);
        else if (hid_devices[i].type == USB_HID_MOUSE)
            usb_hid_poll_mouse(&hid_devices[i]);
    }
}

/* Check if any USB HID keyboard exists */
int usb_has_keyboard(void) {
    for (int i = 0; i < USB_MAX_HID_DEV; i++) {
        if (hid_devices[i].active && hid_devices[i].configured &&
            hid_devices[i].type == USB_HID_KEYBOARD) return 1;
    }
    return 0;
}

/* Check if any USB HID mouse exists */
int usb_has_mouse(void) {
    for (int i = 0; i < USB_MAX_HID_DEV; i++) {
        if (hid_devices[i].active && hid_devices[i].configured &&
            hid_devices[i].type == USB_HID_MOUSE) return 1;
    }
    return 0;
}

/* ============================================================
 * Public API
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

    /* Enumerate all devices and try to initialize HID devices */
    for (int i = 1; i <= XHCI_MAX_SLOTS; i++) {
        if (ctrl->devices[i].active) {
            /* Register in general device list (with bounds check) */
            if (usb_device_count >= USB_MAX_DEVICES) {
                LOG_W("usb", "Too many USB devices, skipping slot %d\n", i);
                continue;
            }
            usb_device_info_t *udev = &usb_devices[usb_device_count];
            memset(udev, 0, sizeof(usb_device_info_t));

            udev->slot_id = ctrl->devices[i].slot_id;
            udev->speed = ctrl->devices[i].speed;
            udev->active = 1;

            /* Try to read device descriptor */
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

            /* Attempt HID initialization for this device */
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
