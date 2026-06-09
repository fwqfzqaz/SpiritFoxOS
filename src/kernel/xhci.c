/* SpiritFoxOS - XHCI主机控制器驱动
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
#include "xhci.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "pic.h"
#include "idt.h"
#include "log.h"
#include "../include/io.h"
#include "../include/string.h"

static xhci_controller_t xhci_ctrl;

#define XHCI_REG8(base, off)  (*(volatile uint8_t *)((uint64_t)(base) + (off)))
#define XHCI_REG16(base, off) (*(volatile uint16_t *)((uint64_t)(base) + (off)))
#define XHCI_REG32(base, off) (*(volatile uint32_t *)((uint64_t)(base) + (off)))
#define XHCI_REG64(base, off) (*(volatile uint64_t *)((uint64_t)(base) + (off)))

#define USBCMD    0x00
#define USBSTS    0x04
#define PAGESIZE  0x08
#define DNCTRL    0x14
#define CRCR      0x18
#define DCBAAP    0x30
#define CONFIG    0x38

#define CAPLENGTH 0x00
#define HCIVERSION 0x02
#define HCSPARAMS1 0x04
#define HCSPARAMS2 0x08
#define HCSPARAMS3 0x0C
#define HCCPARAMS1 0x10
#define DBOFF      0x14
#define RTSOFF     0x18

#define PORTSC_BASE 0x400
#define PORTSC_STRIDE 0x10

#define IMAN    0x00
#define IMOD    0x04
#define ERSTSZ  0x08
#define ERSTBA  0x10
#define ERDP    0x18

/* ---- Ring management ---- */

void xhci_ring_init(xhci_ring_t *ring, uint32_t size) {
    uint64_t pages = (size * sizeof(xhci_trb_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    ring->ring = (xhci_trb_t *)phys_to_virt(phys);
    memset(ring->ring, 0, size * sizeof(xhci_trb_t));
    ring->ring_size = size;
    ring->enqueue_idx = 0;
    ring->cycle_bit = 1;
    ring->ccs = 1;
}

static void xhci_ring_push(xhci_ring_t *ring, xhci_trb_t *trb) {
    xhci_trb_t *dst = &ring->ring[ring->enqueue_idx];
    dst->parameter_low = trb->parameter_low;
    dst->parameter_high = trb->parameter_high;
    dst->status = trb->status;
    uint32_t ctrl = trb->control;
    ctrl &= ~(1 << 0);
    if (ring->cycle_bit) ctrl |= (1 << 0);
    else ctrl &= ~(1 << 0);
    ctrl &= ~TRB_TR_CH;
    dst->control = ctrl;

    ring->enqueue_idx++;
    if (ring->enqueue_idx >= ring->ring_size - 1) {
        xhci_trb_t *link = &ring->ring[ring->enqueue_idx];
        memset(link, 0, sizeof(xhci_trb_t));
        link->parameter_low = (uint32_t)((uint64_t)ring->ring & 0xFFFFFFFF);
        link->parameter_high = (uint32_t)((uint64_t)ring->ring >> 32);
        link->control = TRB_TYPE_LINK << 10 | TRB_TR_CH;
        if (ring->cycle_bit) link->control |= (1 << 0);
        ring->enqueue_idx = 0;
        ring->cycle_bit = !ring->cycle_bit;
    }
}

static void xhci_db_ring(volatile uint32_t *db_reg, uint32_t slot_id, uint32_t target) {
    *db_reg = (slot_id << 8) | target;
}

/* ---- Event handling ---- */

static uint32_t xhci_wait_event(xhci_controller_t *ctrl, xhci_trb_t *out,
                                   int timeout_count) {
    volatile uint64_t *erdp = &XHCI_REG64(ctrl->mmio_base, ctrl->rts_offset + 0x20 + 0x18);

    /* Use controller's event ring consumer state (not static!) */
    uint32_t dequeue_idx = ctrl->event_dequeue_idx;
    uint8_t event_ccs = ctrl->event_ccs;

    for (int timeout = 0; timeout < timeout_count; timeout++) {
        xhci_trb_t *trb = &ctrl->event_ring.ring[dequeue_idx];
        if ((trb->control & 1) == event_ccs) {
            if (out) *out = *trb;
            uint32_t type = (trb->control >> 10) & 0x3F;

            dequeue_idx++;
            if (dequeue_idx >= ctrl->event_ring.ring_size - 1) {
                dequeue_idx = 0;
                event_ccs = !event_ccs;
            }

            /* Save updated state back to controller */
            ctrl->event_dequeue_idx = dequeue_idx;
            ctrl->event_ccs = event_ccs;

            uint64_t new_dequeue = (uint64_t)&ctrl->event_ring.ring[dequeue_idx];
            *erdp = new_dequeue | (1 << 3);

            return type;
        }
        __asm__ volatile("pause");
    }

    /* Timeout: save current position anyway */
    ctrl->event_dequeue_idx = dequeue_idx;
    ctrl->event_ccs = event_ccs;
    return 0;
}

static int xhci_send_command(xhci_controller_t *ctrl, xhci_trb_t *cmd, xhci_trb_t *result) {
    xhci_ring_push(&ctrl->cmd_ring, cmd);
    volatile uint64_t *crcr = &XHCI_REG64(ctrl->mmio_base, ctrl->op_offset + CRCR);
    (void)crcr;
    xhci_db_ring(ctrl->db_reg, 0, 0);

    uint32_t type = xhci_wait_event(ctrl, result, 500000);
    if (type != TRB_TYPE_CMD_COMPLETE) return -1;

    uint32_t completion_code = (result->status >> 24) & 0xFF;
    if (completion_code != 1) return -(int)completion_code;
    return 0;
}

/* ---- Command helpers ---- */

static int xhci_enable_slot(xhci_controller_t *ctrl, uint8_t *slot_id) {
    xhci_trb_t cmd = {0};
    cmd.control = (TRB_TYPE_CMD_ENABLE_SLOT << 10) | TRB_C;
    xhci_trb_t result;
    int rc = xhci_send_command(ctrl, &cmd, &result);
    if (rc == 0) {
        *slot_id = (uint8_t)(result.control >> 24);
    }
    return rc;
}

static int xhci_address_device(xhci_controller_t *ctrl, uint8_t slot_id, uint64_t input_ctx_phys) {
    xhci_trb_t cmd = {0};
    cmd.parameter_low = (uint32_t)(input_ctx_phys & 0xFFFFFFFF);
    cmd.parameter_high = (uint32_t)(input_ctx_phys >> 32);
    cmd.control = (TRB_TYPE_CMD_ADDR_DEV << 10) | (slot_id << 24) | TRB_C;
    xhci_trb_t result;
    return xhci_send_command(ctrl, &cmd, &result);
}

static void xhci_port_reset(xhci_controller_t *ctrl, uint8_t port) {
    volatile uint32_t *portsc = &XHCI_REG32(ctrl->mmio_base, ctrl->op_offset + PORTSC_BASE + port * PORTSC_STRIDE);
    uint32_t val = *portsc;
    if (val & (1 << 4)) {
        *portsc = (val & ~(1 << 4)) | (1 << 4);
    }
    for (int i = 0; i < 100000; i++) {
        val = *portsc;
        if (!(val & (1 << 4))) break;
        __asm__ volatile("pause");
    }
}

/* Set endpoint context pointer in device context at given EP index */
void xhci_dev_ctx_set_ep_ptr(void *dev_ctx, int ep_index, uint64_t tr_phys) {
    volatile uint64_t *ep_ptr = (volatile uint64_t *)((uint8_t *)dev_ctx + 32 + ep_index * 32);
    ep_ptr[0] = tr_phys;
    ep_ptr[1] = 0;
}

/* ---- Device detection ---- */

static void xhci_detect_devices(xhci_controller_t *ctrl) {
    for (int port = 0; port < ctrl->max_ports; port++) {
        volatile uint32_t *portsc = &XHCI_REG32(ctrl->mmio_base, ctrl->op_offset + PORTSC_BASE + port * PORTSC_STRIDE);
        uint32_t val = *portsc;

        if (!(val & (1 << 0))) continue;
        if (!(val & (1 << 1))) continue;

        uint8_t speed = (val >> 10) & 0xF;
        const char *speed_str = "?";
        if (speed == 1) speed_str = "FS";
        else if (speed == 2) speed_str = "LS";
        else if (speed == 3) speed_str = "HS";
        else if (speed == 4) speed_str = "SS";

        LOG_I("xhci", "Port %d: Device connected [%s]\n", port, speed_str);

        xhci_port_reset(ctrl, port);
        val = *portsc;

        uint8_t slot_id;
        if (xhci_enable_slot(ctrl, &slot_id) != 0) {
            LOG_E("xhci", "Failed to enable slot for port %d\n", port);
            continue;
        }

        LOG_I("xhci", "Slot %d enabled for port %d\n", slot_id, port);

        /* Allocate input context (1 page) */
        uint64_t input_ctx_phys = pmm_alloc_pages(1);
        void *input_ctx = (void *)phys_to_virt(input_ctx_phys);
        memset(input_ctx, 0, PAGE_SIZE);

        /* Allocate device context (1 page) */
        uint64_t dev_ctx_phys = pmm_alloc_pages(1);
        void *dev_ctx = (void *)phys_to_virt(dev_ctx_phys);
        memset(dev_ctx, 0, PAGE_SIZE);

        /* Build input context:
         * Offset 0: Input Control Context (64 bytes)
         * Offset 32: Slot Context (32 bytes)
         * Offset 64: EP0 Context (32 bytes)
         */
        uint8_t *slot_ctx = (uint8_t *)input_ctx + 32;
        slot_ctx[0] = speed;       /* Route string = speed field */
        slot_ctx[1] = 0;           /* Context entries = 0 (set by A1 flag) */
        slot_ctx[2] = port + 1;    /* Root hub port number */
        slot_ctx[3] = 0;

        uint8_t *ep0_ctx = (uint8_t *)input_ctx + 64;
        ep0_ctx[0] = 0x07;         /* EP type=4(control), max burst=0, CErr=3 */
        ep0_ctx[1] = 0;            /* Max packet size [7:0] - will be corrected later */
        ep0_ctx[2] = 0;            /* Max packet size [15:8] */
        ep0_ctx[3] = 8;            /* Initial max packet size = 8 bytes */
        ep0_ctx[4] = 1;            /* Max PStreams=0, interval=0, mult=0 */

        /* Input control context: set A0(slot) and A1(ep0) flags */
        uint64_t *ictrl = (uint64_t *)input_ctx;
        ictrl[0] = 0;              /* Drop context flags = 0 */
        ictrl[1] = 3;              /* Add context flags: bit0=slot, bit1=ep0 */

        /* Register device context in DCBAAP */
        ctrl->dcbaa[slot_id] = dev_ctx_phys;
        ctrl->dcbaa_contexts[slot_id] = dev_ctx;

        if (xhci_address_device(ctrl, slot_id, input_ctx_phys) != 0) {
            LOG_E("xhci", "Failed to address device on slot %d\n", slot_id);
            continue;
        }

        xhci_device_t *dev = &ctrl->devices[slot_id];
        dev->port = port;
        dev->slot_id = slot_id;
        dev->speed = speed;
        dev->active = 1;
        dev->input_ctx = input_ctx;
        dev->device_ctx = dev_ctx;

        /* Initialize EP0 transfer ring and point device context to it */
        xhci_ring_init(&dev->transfer_rings[1], XHCI_RING_SIZE);  /* index 1 = EP0 */
        xhci_dev_ctx_set_ep_ptr(dev_ctx, 1,
                           virt_to_phys((uint64_t)dev->transfer_rings[1].ring));

        LOG_I("xhci", "Device addressed on slot %d port %d\n", slot_id, port);
    }
}

/* ============================================================
 * Control Transfer on EP0
 * Sends SETUP [+ DATA_IN/OUT] + STATUS TRB chain.
 * ============================================================ */

int xhci_control_transfer(xhci_controller_t *ctrl, uint8_t slot_id,
                          uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                          void *data_buf) {
    if (!ctrl || !ctrl->initialized || slot_id < 1 || slot_id > XHCI_MAX_SLOTS)
        return -1;
    if (!ctrl->devices[slot_id].active) return -1;

    xhci_device_t *dev = &ctrl->devices[slot_id];
    xhci_ring_t *ep0_ring = &dev->transfer_rings[1];  /* EP0 = ring index 1 */

    int direction_in = (bmRequestType & 0x80) ? 1 : 0;
    int has_data_stage = (wLength > 0 && data_buf != NULL) ? 1 : 0;

    /* SETUP stage TRB */
    xhci_trb_t setup = {0};
    setup.parameter_low = bmRequestType | (bRequest << 8) |
                          ((wValue & 0xFF) << 16);
    setup.parameter_high = ((wValue >> 8) & 0xFF) |
                           ((wIndex & 0xFF) << 8) | ((wIndex >> 8) << 24);
    setup.status = wLength;          /* wLength in setup packet */
    setup.control = (TRB_TYPE_SETUP_STAGE << 10) | TRB_IDT;
    /* TRT bits [17:16]: 0=no data stage, 2=IN, 3=OUT */
    if (has_data_stage)
        setup.control |= direction_in ? (2u << 16) : (3u << 16);

    /* DATA stage TRB (optional) */
    xhci_trb_t data = {0};
    if (has_data_stage) {
        uint64_t buf_phys = virt_to_phys((uint64_t)data_buf);
        data.parameter_low = (uint32_t)(buf_phys & 0xFFFFFFFF);
        data.parameter_high = (uint32_t)(buf_phys >> 32);
        data.status = wLength;
        data.control = (TRB_TYPE_DATA_STAGE << 10) | TRB_IOC;
        data.control |= direction_in ? (1u << 16) : (2u << 16);  /* IN/OUT dir */
    }

    /* STATUS stage TRB: direction opposite to DATA */
    xhci_trb_t status = {0};
    status.control = (TRB_TYPE_STATUS_STAGE << 10) | TRB_IOC;
    status.control |= direction_in ? (2u << 16) : (1u << 16);  /* OUT/IN */

    /* Push all TRBs onto EP0 transfer ring */
    xhci_ring_push(ep0_ring, &setup);
    if (has_data_stage)
        xhci_ring_push(ep0_ring, &data);
    xhci_ring_push(ep0_ring, &status);

    /* Ring doorbell for EP0 (target=1) */
    xhci_db_ring(ctrl->db_reg, slot_id, 1);

    /* Wait for completion event */
    xhci_trb_t evt;
    uint32_t evt_type = xhci_wait_event(ctrl, &evt, 500000);

    if (evt_type != TRB_TYPE_TRANSFER_EVENT) {
        return -2;
    }

    uint32_t comp_code = (evt.status >> 24) & 0xFF;
    if (comp_code == 1) {  /* Success */
        /* For IN transfers, actual length = requested - residual */
        if (direction_in && has_data_stage) {
            return (int)(wLength - (evt.status & 0xFFFFFF));
        }
        return 0;
    }

    return -(int)comp_code;
}

/* ============================================================
 * Configure Endpoint command
 * ============================================================ */

int xhci_configure_endpoint(xhci_controller_t *ctrl, uint8_t slot_id,
                            uint64_t input_ctx_phys) {
    if (!ctrl || !ctrl->initialized) return -1;

    xhci_trb_t cmd = {0};
    cmd.parameter_low = (uint32_t)(input_ctx_phys & 0xFFFFFFFF);
    cmd.parameter_high = (uint32_t)(input_ctx_phys >> 32);
    cmd.control = (TRB_TYPE_CMD_CONFIG_EP << 10) | (slot_id << 24) | TRB_C;

    xhci_trb_t result;
    return xhci_send_command(ctrl, &cmd, &result);
}

/* ============================================================
 * Evaluate Context command
 * ============================================================ */

int xhci_evaluate_context(xhci_controller_t *ctrl, uint8_t slot_id,
                           uint64_t input_ctx_phys) {
    xhci_trb_t cmd = {0};
    cmd.parameter_low = (uint32_t)(input_ctx_phys & 0xFFFFFFFF);
    cmd.parameter_high = (uint32_t)(input_ctx_phys >> 32);
    cmd.control = (TRB_TYPE_CMD_EVAL_CTX << 10) | (slot_id << 24) | TRB_C;

    xhci_trb_t result;
    return xhci_send_command(ctrl, &cmd, &result);
}

/* ============================================================
 * Normal / Interrupt Transfer on a non-EP0 endpoint
 * ============================================================ */

int xhci_transfer_data(xhci_controller_t *ctrl, uint8_t slot_id,
                       uint8_t ep_index, void *buf, uint32_t len,
                       int direction_in) {
    if (!ctrl || !ctrl->initialized || slot_id < 1 || slot_id > XHCI_MAX_SLOTS)
        return -1;
    if (!ctrl->devices[slot_id].active) return -1;
    if (ep_index < 2 || ep_index >= 4) return -1;

    xhci_device_t *dev = &ctrl->devices[slot_id];
    xhci_ring_t *ring = &dev->transfer_rings[ep_index];

    uint64_t buf_phys = virt_to_phys((uint64_t)buf);

    xhci_trb_t trb = {0};
    trb.parameter_low = (uint32_t)(buf_phys & 0xFFFFFFFF);
    trb.parameter_high = (uint32_t)(buf_phys >> 32);
    trb.status = len;
    trb.control = (TRB_TYPE_NORMAL << 10) | TRB_IOC;
    trb.control |= direction_in ? (1u << 16) : (2u << 16);

    xhci_ring_push(ring, &trb);

    /* Ring doorbell for this endpoint */
    xhci_db_ring(ctrl->db_reg, slot_id, ep_index);

    /* Wait for transfer event (short timeout for polling) */
    xhci_trb_t evt;
    uint32_t evt_type = xhci_wait_event(ctrl, &evt, 1000);  /* Short timeout for polling */

    if (evt_type != TRB_TYPE_TRANSFER_EVENT) {
        return -2;
    }

    uint32_t comp_code = (evt.status >> 24) & 0xFF;
    if (comp_code == 1) {
        return (int)(len - (evt.status & 0xFFFFFF));  /* actual = req - residual */
    }

    return -(int)comp_code;
}

/* ============================================================
 * Controller initialization
 * ============================================================ */

int xhci_init(pci_device_t *pci_dev) {
    memset(&xhci_ctrl, 0, sizeof(xhci_controller_t));
    xhci_ctrl.pci_dev = *pci_dev;

    pci_enable_device(pci_dev);

    xhci_ctrl.mmio_phys = pci_dev->bar[0];
    xhci_ctrl.mmio_size = pci_dev->bar_size[0];

    if (xhci_ctrl.mmio_phys == 0) {
        LOG_E("xhci", "No MMIO BAR found\n");
        return -1;
    }

    uint64_t mmio_pages = (xhci_ctrl.mmio_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < mmio_pages; i++) {
        uint64_t virt = xhci_ctrl.mmio_phys + i * PAGE_SIZE;
        uint64_t phys = virt;
        vmm_map_page(vmm_get_kernel_pml4(), virt, phys,
                     PTE_PRESENT | PTE_WRITABLE | PTE_ACCESSED | PTE_DIRTY);
    }

    xhci_ctrl.mmio_base = (volatile uint8_t *)xhci_ctrl.mmio_phys;

    xhci_ctrl.cap_length = XHCI_REG8(xhci_ctrl.mmio_base, CAPLENGTH);
    xhci_ctrl.hci_version = XHCI_REG16(xhci_ctrl.mmio_base, HCIVERSION);
    xhci_ctrl.op_offset = xhci_ctrl.cap_length;

    uint32_t hcs1 = XHCI_REG32(xhci_ctrl.mmio_base, HCSPARAMS1);
    xhci_ctrl.max_slots = (uint8_t)(hcs1 & 0xFF);
    xhci_ctrl.max_ports = (uint8_t)((hcs1 >> 24) & 0xFF);
    xhci_ctrl.max_intrs = (uint16_t)((hcs1 >> 8) & 0x3FF);

    uint32_t dboff = XHCI_REG32(xhci_ctrl.mmio_base, DBOFF);
    xhci_ctrl.db_reg = (volatile uint32_t *)((uint64_t)xhci_ctrl.mmio_base + dboff);

    xhci_ctrl.rts_offset = XHCI_REG32(xhci_ctrl.mmio_base, RTSOFF);

    LOG_I("xhci", "Version %x.%x, Slots=%d, Ports=%d\n",
               xhci_ctrl.hci_version >> 8, xhci_ctrl.hci_version & 0xFF,
               xhci_ctrl.max_slots, xhci_ctrl.max_ports);

    volatile uint32_t *usbsts = &XHCI_REG32(xhci_ctrl.mmio_base, xhci_ctrl.op_offset + USBSTS);
    volatile uint32_t *usbcmd = &XHCI_REG32(xhci_ctrl.mmio_base, xhci_ctrl.op_offset + USBCMD);

    /* Stop controller if running */
    if (!(*usbsts & (1 << 0))) {
        *usbcmd &= ~1;
        for (int i = 0; i < 100000; i++) {
            if (*usbsts & (1 << 0)) break;
            __asm__ volatile("pause");
        }
    }

    /* Reset controller */
    *usbcmd |= (1 << 1);
    for (int i = 0; i < 100000; i++) {
        if (!(*usbcmd & (1 << 1))) break;
        __asm__ volatile("pause");
    }
    for (int i = 0; i < 100000; i++) {
        if (!(*usbsts & (1 << 11))) break;
        __asm__ volatile("pause");
    }

    /* Set number of device slots enabled */
    volatile uint32_t *config = &XHCI_REG32(xhci_ctrl.mmio_base, xhci_ctrl.op_offset + CONFIG);
    *config = (*config & 0xFFFFFF00) | (xhci_ctrl.max_slots & 0xFF);

    /* Setup DCBAAP (Device Context Base Address Array Pointer) */
    uint64_t dcbaa_pages = ((XHCI_MAX_SLOTS + 1) * sizeof(uint64_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t dcbaa_phys = pmm_alloc_pages(dcbaa_pages);
    xhci_ctrl.dcbaa = (uint64_t *)phys_to_virt(dcbaa_phys);
    memset(xhci_ctrl.dcbaa, 0, (XHCI_MAX_SLOTS + 1) * sizeof(uint64_t));

    volatile uint64_t *dcbaap = &XHCI_REG64(xhci_ctrl.mmio_base, xhci_ctrl.op_offset + DCBAAP);
    *dcbaap = dcbaa_phys;

    /* Command ring */
    xhci_ring_init(&xhci_ctrl.cmd_ring, XHCI_RING_SIZE);
    volatile uint64_t *crcr = &XHCI_REG64(xhci_ctrl.mmio_base, xhci_ctrl.op_offset + CRCR);
    *crcr = ((uint64_t)virt_to_phys((uint64_t)xhci_ctrl.cmd_ring.ring)) | 1;

    /* Event ring */
    xhci_ring_init(&xhci_ctrl.event_ring, XHCI_EVENT_RING_SIZE);

    uint64_t erst_phys = pmm_alloc_pages(1);
    xhci_ctrl.erst = (xhci_erst_entry_t *)phys_to_virt(erst_phys);
    memset(xhci_ctrl.erst, 0, sizeof(xhci_erst_entry_t));
    xhci_ctrl.erst->ring_addr = virt_to_phys((uint64_t)xhci_ctrl.event_ring.ring);
    xhci_ctrl.erst->size = XHCI_EVENT_RING_SIZE;

    volatile uint32_t *erstsz = &XHCI_REG32(xhci_ctrl.mmio_base, xhci_ctrl.rts_offset + 0x20 + ERSTSZ);
    *erstsz = 1;

    volatile uint64_t *erstba = &XHCI_REG64(xhci_ctrl.mmio_base, xhci_ctrl.rts_offset + 0x20 + ERSTBA);
    *erstba = erst_phys;

    volatile uint64_t *erdp = &XHCI_REG64(xhci_ctrl.mmio_base, xhci_ctrl.rts_offset + 0x20 + ERDP);
    *erdp = virt_to_phys((uint64_t)xhci_ctrl.event_ring.ring);

    /* Enable interrupter */
    volatile uint32_t *iman = &XHCI_REG32(xhci_ctrl.mmio_base, xhci_ctrl.rts_offset + 0x20 + IMAN);
    *iman |= 3;

    /* Start controller (Run/Stop = 1) */
    *usbcmd |= 1;
    for (int i = 0; i < 100000; i++) {
        if (!(*usbsts & (1 << 0))) break;
        __asm__ volatile("pause");
    }

    xhci_ctrl.initialized = 1;
    xhci_ctrl.running = 1;

    /* Initialize event ring consumer state */
    xhci_ctrl.event_dequeue_idx = 0;
    xhci_ctrl.event_ccs = 1;  /* Event ring starts with cycle bit = 1 */

    LOG_I("xhci", "Controller started successfully\n");

    xhci_detect_devices(&xhci_ctrl);

    return 0;
}

void xhci_handler(struct interrupt_frame *frame) {
    (void)frame;
    volatile uint32_t *iman = &XHCI_REG32(xhci_ctrl.mmio_base, xhci_ctrl.rts_offset + 0x20 + IMAN);
    *iman |= 1;

    pic_send_eoi(11);
}

xhci_controller_t *xhci_get_controller(void) {
    if (xhci_ctrl.initialized) return &xhci_ctrl;
    return NULL;
}

int xhci_get_device_count(void) {
    int count = 0;
    for (int i = 1; i <= XHCI_MAX_SLOTS; i++) {
        if (xhci_ctrl.devices[i].active) count++;
    }
    return count;
}

xhci_device_t *xhci_get_device_info(int slot_id) {
    if (slot_id < 1 || slot_id > XHCI_MAX_SLOTS) return NULL;
    if (!xhci_ctrl.devices[slot_id].active) return NULL;
    return &xhci_ctrl.devices[slot_id];
}
