/* SpiritFoxOS - USB核心接口
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
#ifndef USB_H
#define USB_H

#include <stdint.h>

#define USB_CLASS_HID       0x03
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_CLASS_HUB       0x09

#define USB_REQ_GET_STATUS  0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE 0x03
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_CONFIG  0x09

#define USB_DESC_DEVICE     0x01
#define USB_DESC_CONFIG     0x02
#define USB_DESC_STRING     0x03
#define USB_DESC_INTERFACE  0x04
#define USB_DESC_ENDPOINT   0x05

#define USB_HID_REQ_GET_REPORT 0x01
#define USB_HID_REQ_SET_REPORT 0x09
#define USB_HID_REQ_SET_PROTOCOL 0x0B

#define USB_HID_BOOT_PROTOCOL 0x00
#define USB_HID_REPORT_PROTOCOL 0x01

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_interface_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

typedef struct {
    uint8_t  slot_id;
    uint8_t  address;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  num_configs;
    uint8_t  num_interfaces;
    uint8_t  active;
    uint8_t  speed;
    uint8_t  hid_type;
    uint8_t  hid_endpoint_addr;
    uint16_t hid_max_packet;
    uint8_t  hid_interval;
} usb_device_info_t;

#define USB_MAX_DEVICES 32

void usb_init(void);
int usb_get_device_list(usb_device_info_t *list, int max);
int usb_hid_set_boot_protocol(uint8_t slot_id, uint8_t interface_num,
                               uint8_t protocol);
int usb_hid_get_report(uint8_t slot_id, uint8_t *buf, uint16_t len);
const char *usb_class_name(uint8_t class_code);
const char *usb_speed_name(uint8_t speed);

/* USB HID轮询（从GUI主循环调用） */
void usb_hid_poll_all(void);

/* 检查USB HID键盘/鼠标设备是否可用 */
int usb_has_keyboard(void);
int usb_has_mouse(void);

#endif
