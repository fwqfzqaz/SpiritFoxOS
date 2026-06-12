/* SpiritFoxOS - XHCI主机控制器接口
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
#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>
#include "pci.h"
#include "idt.h"

#define XHCI_MAX_SLOTS   32
#define XHCI_RING_SIZE   256
#define XHCI_EVENT_RING_SEGS 1
#define XHCI_EVENT_RING_SIZE 256

#define TRB_TYPE_NORMAL          1
#define TRB_TYPE_SETUP_STAGE    2
#define TRB_TYPE_DATA_STAGE     3
#define TRB_TYPE_STATUS_STAGE   4
#define TRB_TYPE_LINK           6
#define TRB_TYPE_CMD_ENABLE_SLOT 9
#define TRB_TYPE_CMD_ADDR_DEV   11
#define TRB_TYPE_CMD_CONFIG_EP  12
#define TRB_TYPE_CMD_EVAL_CTX   13
#define TRB_TYPE_CMD_RESET_DEV  18
#define TRB_TYPE_PORT_STATUS    34
#define TRB_TYPE_TRANSFER_EVENT 32
#define TRB_TYPE_CMD_COMPLETE   33

#define TRB_C (1 << 0)
#define TRB_TR_CH (1 << 1)
#define TRB_IOC (1 << 5)
#define TRB_IDT (1 << 6)

typedef struct {
    uint32_t parameter_low;
    uint32_t parameter_high;
    uint32_t status;
    uint32_t control;
} __attribute__((packed, aligned(16))) xhci_trb_t;

typedef struct {
    uint64_t ring_addr;
    uint16_t size;
    uint16_t reserved;
    uint32_t reserved2;
} __attribute__((packed, aligned(64))) xhci_erst_entry_t;

typedef struct {
    volatile uint32_t *db_reg;
    xhci_trb_t *ring;
    uint32_t enqueue_idx;
    uint32_t ring_size;
    uint8_t  cycle_bit;
    uint8_t  ccs;
} xhci_ring_t;

typedef struct {
    uint8_t  port;
    uint8_t  slot_id;
    uint8_t  speed;
    uint8_t  address;
    uint8_t  active;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint16_t vendor_id;
    uint16_t device_id;
    xhci_ring_t transfer_rings[4];
    void *input_ctx;
    void *device_ctx;
} xhci_device_t;

typedef struct {
    pci_device_t pci_dev;
    volatile uint8_t *mmio_base;
    uint64_t mmio_phys;
    uint32_t mmio_size;

    uint8_t  cap_length;
    uint16_t hci_version;
    uint8_t  max_slots;
    uint8_t  max_ports;
    uint16_t max_intrs;
    uint32_t op_offset;
    uint32_t rts_offset;
    volatile uint32_t *db_reg;

    xhci_ring_t cmd_ring;
    xhci_ring_t event_ring;
    xhci_erst_entry_t *erst;

    uint64_t *dcbaa;
    void     *dcbaa_contexts[XHCI_MAX_SLOTS + 1];

    xhci_device_t devices[XHCI_MAX_SLOTS + 1];

    int initialized;
    int running;

    /* 事件环消费端状态（替代静态变量） */
    uint32_t event_dequeue_idx;
    uint8_t  event_ccs;
} xhci_controller_t;

int xhci_init(pci_device_t *pci_dev);
void xhci_handler(struct interrupt_frame *frame);
xhci_controller_t *xhci_get_controller(void);
int xhci_get_device_count(void);
xhci_device_t *xhci_get_device_info(int slot_id);

/* EP0上的控制传输 */
int xhci_control_transfer(xhci_controller_t *ctrl, uint8_t slot_id,
                          uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                          void *data_buf);

/* 配置端点（CONFIG_EP命令） */
int xhci_configure_endpoint(xhci_controller_t *ctrl, uint8_t slot_id,
                            uint64_t input_ctx_phys);

/* 评估上下文（EVAL_CTX命令） */
int xhci_evaluate_context(xhci_controller_t *ctrl, uint8_t slot_id,
                           uint64_t input_ctx_phys);

/* 非EP0端点上的常规/中断数据传输 */
int xhci_transfer_data(xhci_controller_t *ctrl, uint8_t slot_id,
                       uint8_t ep_index, void *buf, uint32_t len,
                       int direction_in);

/* 环管理（USB HID端点配置需要） */
void xhci_ring_init(xhci_ring_t *ring, uint32_t size);
void xhci_dev_ctx_set_ep_ptr(void *dev_ctx, int ep_index, uint64_t tr_phys);

#endif
