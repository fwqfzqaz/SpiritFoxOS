/*
 * rtl8139.c - RTL8139 Ethernet driver for SpiritFoxOS
 *
 * Driver for the Realtek RTL8139 Fast Ethernet controller,
 * which is QEMU's default NIC (-net nic).
 * Uses PIO (Port I/O) mode via BAR0.
 *
 * Register layout per Realtek RTL8139C(L) datasheet / Linux 8139too.c:
 *   0x00  MAC0-5        (6 bytes, R/O)   Ethernet hardware address
 *   0x08  MAR0-7        (8 bytes, R/W)   Multicast hash
 *   0x10  TSD0-TSD3     (4x4 bytes, R/W) Transmit Status Descriptors
 *   0x20  TSAD0-TSAD3   (4x4 bytes, R/W) Transmit Start Address Descriptors
 *   0x30  RBSTART       (4 bytes, R/W)   Receive Buffer Start Address
 *   0x37  ChipCmd       (1 byte, R/W)    Command Register
 *   0x38  RxBufPtr      (2 bytes, R/W)   Current Address of Packet Read (CAPR)
 *   0x3A  RxBufAddr     (2 bytes, R/O)   Current Buffer Address (CBR)
 *   0x3C  IntrMask      (2 bytes, R/W)   Interrupt Mask
 *   0x3E  IntrStatus    (2 bytes, R/W)   Interrupt Status
 *   0x40  TxConfig      (4 bytes, R/W)   Transmit Configuration
 *   0x44  RxConfig      (4 bytes, R/W)   Receive Configuration
 *   0x52  Config1       (1 byte, R/W)    Configuration Register 1
 */

#include "rtl8139.h"
#include "pci.h"
#include "hal.h"
#include "memory.h"
#include "apic.h"
#include "net.h"
#include "string.h"
#include "vga.h"

/* ========================================================================
 * RTL8139 PCI IDs
 * ======================================================================== */
#define RTL8139_VENDOR_ID  0x10EC
#define RTL8139_DEVICE_ID  0x8139

/* ========================================================================
 * RTL8139 I/O register offsets (relative to BAR0 base)
 * ======================================================================== */
#define RTL_REG_MAC0       0x00    /* MAC address bytes 0-5 (R/O) */
#define RTL_REG_MAR0       0x08    /* Multicast hash (8 bytes, R/W) */
#define RTL_REG_TSD0       0x10    /* TX Status Descriptor 0 (32-bit, R/W) */
#define RTL_REG_TSD1       0x14    /* TX Status Descriptor 1 */
#define RTL_REG_TSD2       0x18    /* TX Status Descriptor 2 */
#define RTL_REG_TSD3       0x1C    /* TX Status Descriptor 3 */
#define RTL_REG_TSAD0      0x20    /* TX Start Address 0 (32-bit, R/W) */
#define RTL_REG_TSAD1      0x24    /* TX Start Address 1 */
#define RTL_REG_TSAD2      0x28    /* TX Start Address 2 */
#define RTL_REG_TSAD3      0x2C    /* TX Start Address 3 */
#define RTL_REG_RBSTART    0x30    /* RX Buffer Start Address (32-bit, R/W) */
#define RTL_REG_CMD        0x37    /* Command Register (8-bit, R/W) */
#define RTL_REG_CAPR       0x38    /* Current Address of Packet Read (16-bit, R/W) */
#define RTL_REG_CBR        0x3A    /* Current Buffer Address (16-bit, R/O) */
#define RTL_REG_IMR        0x3C    /* Interrupt Mask (16-bit, R/W) */
#define RTL_REG_ISR        0x3E    /* Interrupt Status (16-bit, R/W) */
#define RTL_REG_TXCFG      0x40    /* Transmit Configuration (32-bit, R/W) */
#define RTL_REG_RXCFG      0x44    /* Receive Configuration (32-bit, R/W) */
#define RTL_REG_CONFIG1    0x52    /* Configuration Register 1 (8-bit, R/W) */

/* ========================================================================
 * ChipCmd (0x37) bits
 * ======================================================================== */
#define RTL_CMD_BUFE       (1 << 0)   /* RX Buffer Empty (R/O) */
#define RTL_CMD_RE         (1 << 2)   /* Receiver Enable */
#define RTL_CMD_TE         (1 << 3)   /* Transmitter Enable */
#define RTL_CMD_RST        (1 << 4)   /* Software Reset */

/* ========================================================================
 * ISR / IMR bits (0x3E / 0x3C)
 * ======================================================================== */
#define RTL_INT_RX_OK      (1 << 0)   /* Packet received OK */
#define RTL_INT_RX_ERR     (1 << 1)   /* Receive error */
#define RTL_INT_TX_OK      (1 << 2)   /* Packet transmitted OK */
#define RTL_INT_TX_ERR     (1 << 3)   /* Transmit error */
#define RTL_INT_RX_FIFO    (1 << 4)   /* RX FIFO overflow */
#define RTL_INT_LINK       (1 << 5)   /* Link change */
#define RTL_INT_RX_UNDERRUN (1 << 6)  /* RX underrun */
#define RTL_INT_RX_OVERFLOW (1 << 7)  /* RX buffer overflow */
#define RTL_INT_TIMEOUT    (1 << 14)  /* Cable length / timeout */
#define RTL_INT_SYSERR     (1 << 15)  /* System error */

/* ========================================================================
 * TSD (TX Status Descriptor) bits
 * ======================================================================== */
#define RTL_TSD_OWN        (1 << 13)  /* OWN: set 1 to start TX, HW clears on done */
#define RTL_TSD_TOK        (1 << 15)  /* TX OK */
#define RTL_TSD_TUN        (1 << 14)  /* TX FIFO underrun */
#define RTL_TSD_TABT       (1 << 18)  /* Transmit abort */

/* ========================================================================
 * RX configuration
 * ======================================================================== */
/* Accept broadcast + multicast + physical match + perfect match,
 * max DMA burst (0xF << 8), wrap mode */
#define RTL_RXCFG_VAL     0x00000F0E

/* ========================================================================
 * TX configuration
 * ======================================================================== */
/* IFG standard (0x3 << 24), max DMA burst 256 bytes */
#define RTL_TXCFG_VAL     0x03000000

/* ========================================================================
 * RX ring buffer constants
 * ======================================================================== */
#define RTL_RX_BUF_SIZE   8192       /* 8K ring buffer */
#define RTL_RX_BUF_PAD    16         /* 16 bytes padding for wrap */
#define RTL_RX_BUF_EXTRA  1500       /* Extra for wrapping max-size frame */
#define RTL_RX_BUF_TOTAL  (RTL_RX_BUF_SIZE + RTL_RX_BUF_PAD + RTL_RX_BUF_EXTRA)

/* TX buffer size (per descriptor) */
#define RTL_TX_BUF_SIZE   1792

/* Number of TX descriptors */
#define RTL_TX_DESCRIPTORS 4

/* ========================================================================
 * Driver state
 * ======================================================================== */
static uint16_t  rtl_io_base = 0;
static uint8_t   rtl_irq = 0;
static uint8_t   rtl_mac[6];
static uint8_t  *rx_buffer = NULL;
static uint8_t  *tx_buffers[RTL_TX_DESCRIPTORS] = {NULL};
static uint32_t  tx_current = 0;
static int       rtl_initialized = 0;

/* ========================================================================
 * I/O helper wrappers
 * ======================================================================== */

static inline uint8_t rtl_inb(uint16_t offset)
{
    return hal_inb(rtl_io_base + offset);
}

static inline uint16_t rtl_inw(uint16_t offset)
{
    return hal_inw(rtl_io_base + offset);
}

static inline uint32_t rtl_inl(uint16_t offset)
{
    return hal_inl(rtl_io_base + offset);
}

static inline void rtl_outb(uint16_t offset, uint8_t val)
{
    hal_outb(rtl_io_base + offset, val);
}

static inline void rtl_outw(uint16_t offset, uint16_t val)
{
    hal_outw(rtl_io_base + offset, val);
}

static inline void rtl_outl(uint16_t offset, uint32_t val)
{
    hal_outl(rtl_io_base + offset, val);
}

/* ========================================================================
 * PCI device discovery
 * ======================================================================== */

static const pci_device_t *rtl8139_find_pci(void)
{
    int count = pci_get_device_count();
    for (int i = 0; i < count; i++) {
        const pci_device_t *dev = pci_get_device(i);
        if (dev && dev->vendor_id == RTL8139_VENDOR_ID &&
                   dev->device_id == RTL8139_DEVICE_ID) {
            return dev;
        }
    }
    return NULL;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void rtl8139_init(void)
{
    /* Step 1: Find the RTL8139 on the PCI bus */
    const pci_device_t *pcidev = rtl8139_find_pci();
    if (!pcidev) {
        printf("[RTL8139] Device not found on PCI bus\n");
        return;
    }

    printf("[RTL8139] Found at PCI %02x:%02x.%x\n",
           pcidev->bus, pcidev->device, pcidev->function);

    /* Step 2: Enable PCI bus mastering and I/O space access */
    uint32_t cmd = pci_read_config(pcidev->bus, pcidev->device,
                                    pcidev->function, 0x04);
    pci_write_config(pcidev->bus, pcidev->device, pcidev->function,
                     0x04, cmd | 0x05);  /* I/O Space Enable + Bus Master Enable */

    /* Step 3: Read BAR0 for I/O base address */
    uint32_t bar0 = pci_read_config(pcidev->bus, pcidev->device,
                                     pcidev->function, 0x10);
    if (!(bar0 & 0x01)) {
        printf("[RTL8139] BAR0 is not I/O space (bar0=0x%x)\n", bar0);
        return;
    }
    rtl_io_base = (uint16_t)(bar0 & ~0x03);  /* Mask low 2 bits */
    printf("[RTL8139] I/O base: 0x%x\n", rtl_io_base);

    /* Step 4: Read IRQ line */
    rtl_irq = pcidev->interrupt_line;
    if (rtl_irq == 0 || rtl_irq == 0xFF) {
        printf("[RTL8139] No IRQ assigned, defaulting to IRQ 11\n");
        rtl_irq = 11;
    }
    printf("[RTL8139] IRQ: %u\n", rtl_irq);

    /* Step 5: Power on the card (clear LWACT+LWPTN bits in CONFIG1) */
    rtl_outb(RTL_REG_CONFIG1, 0x00);

    /* Step 6: Software reset */
    rtl_outb(RTL_REG_CMD, RTL_CMD_RST);
    int timeout = 1000;
    while ((rtl_inb(RTL_REG_CMD) & RTL_CMD_RST) && timeout > 0) {
        hal_io_wait();
        timeout--;
    }
    if (timeout == 0) {
        printf("[RTL8139] Reset timed out!\n");
        return;
    }
    printf("[RTL8139] Reset complete\n");

    /* Step 7: Read MAC address (6 bytes at 0x00) */
    for (int i = 0; i < 6; i++) {
        rtl_mac[i] = rtl_inb(RTL_REG_MAC0 + i);
    }
    printf("[RTL8139] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           rtl_mac[0], rtl_mac[1], rtl_mac[2],
           rtl_mac[3], rtl_mac[4], rtl_mac[5]);

    /* Step 8: Allocate RX buffer (must be 16-byte aligned).
     * Kernel uses identity mapping, so virt == phys.
     * 3 pages = 12288 bytes >= 8192 + 16 + 1500 = 9708 bytes. */
    rx_buffer = (uint8_t *)alloc_pages(3);
    if (!rx_buffer) {
        printf("[RTL8139] Failed to allocate RX buffer\n");
        return;
    }
    memset(rx_buffer, 0, RTL_RX_BUF_TOTAL);

    /* Write physical address of RX buffer to RBSTART */
    rtl_outl(RTL_REG_RBSTART, (uint32_t)(uintptr_t)rx_buffer);

    /* Step 9: Allocate TX buffers (4 descriptors, one page each) */
    for (int i = 0; i < RTL_TX_DESCRIPTORS; i++) {
        tx_buffers[i] = (uint8_t *)alloc_page();
        if (!tx_buffers[i]) {
            printf("[RTL8139] Failed to allocate TX buffer %d\n", i);
            return;
        }
    }

    /* Step 10: Set TX configuration */
    rtl_outl(RTL_REG_TXCFG, RTL_TXCFG_VAL);

    /* Step 11: Set RX configuration (accept broadcast + multicast +
     * physical match + perfect match, max DMA burst, wrap mode) */
    rtl_outl(RTL_REG_RXCFG, RTL_RXCFG_VAL);

    /* Step 12: Clear all pending interrupts */
    rtl_outw(RTL_REG_ISR, 0xFFFF);

    /* Step 13: Enable interrupts: RX_OK | TX_OK | RX_ERR | TX_ERR | RX_OVERFLOW */
    rtl_outw(RTL_REG_IMR, RTL_INT_RX_OK | RTL_INT_TX_OK |
                          RTL_INT_RX_ERR | RTL_INT_TX_ERR |
                          RTL_INT_RX_OVERFLOW);

    /* Step 14: Enable receiver and transmitter */
    rtl_outb(RTL_REG_CMD, RTL_CMD_RE | RTL_CMD_TE);

    /* Step 15: Route IRQ through IOAPIC */
    uint8_t vector = 32 + rtl_irq;
    apic_enable_irq(rtl_irq, vector);

    printf("[RTL8139] Initialized (vector %u, I/O base 0x%x)\n", vector, rtl_io_base);

    rtl_initialized = 1;
}

int rtl8139_get_mac(uint8_t mac[6])
{
    if (!rtl_initialized)
        return -1;
    memcpy(mac, rtl_mac, 6);
    return 0;
}

void rtl8139_send(const void *data, size_t len)
{
    if (!rtl_initialized || !data || len == 0)
        return;

    /* Clamp to max TX frame size */
    if (len > RTL_TX_BUF_SIZE)
        len = RTL_TX_BUF_SIZE;

    /* Find a free TX descriptor (OWN bit cleared by HW after TX complete) */
    int timeout = 1000000;
    uint32_t tsd;
    do {
        tsd = rtl_inl(RTL_REG_TSD0 + tx_current * 4);
        if (!(tsd & RTL_TSD_OWN))
            break;
        hal_io_wait();
        timeout--;
    } while (timeout > 0);

    if (timeout == 0) {
        /* All descriptors busy, try advancing */
        tx_current = (tx_current + 1) % RTL_TX_DESCRIPTORS;
        tsd = rtl_inl(RTL_REG_TSD0 + tx_current * 4);
        if (tsd & RTL_TSD_OWN) {
            printf("[RTL8139] TX timeout - all descriptors busy\n");
            return;
        }
    }

    /* Copy packet data to the TX buffer */
    memcpy(tx_buffers[tx_current], data, len);

    /* Write TX start address */
    rtl_outl(RTL_REG_TSAD0 + tx_current * 4,
             (uint32_t)(uintptr_t)tx_buffers[tx_current]);

    /* Write TX status descriptor: size | OWN bit to trigger transmission */
    rtl_outl(RTL_REG_TSD0 + tx_current * 4, (uint32_t)len | RTL_TSD_OWN);

    /* Advance to next descriptor */
    tx_current = (tx_current + 1) % RTL_TX_DESCRIPTORS;
}

void rtl8139_irq_handler(void)
{
    if (!rtl_initialized)
        return;

    /* Read interrupt status */
    uint16_t status = rtl_inw(RTL_REG_ISR);

    if (status == 0) {
        /* Not our interrupt */
        return;
    }

    /* Handle RX: process all available packets in the ring buffer */
    if (status & (RTL_INT_RX_OK | RTL_INT_RX_ERR | RTL_INT_RX_OVERFLOW)) {
        while (!(rtl_inb(RTL_REG_CMD) & RTL_CMD_BUFE)) {
            /* CAPR stores (current_read_position - 0x10).
             * After reset CAPR = 0xFFF0, so initial read offset = 0xFFF0 + 16 = 0x10000 = 0 mod 8K. */
            uint16_t capr = rtl_inw(RTL_REG_CAPR);
            uint32_t read_offset = (uint32_t)(capr + 16) % RTL_RX_BUF_SIZE;

            /* Read the 4-byte packet header from the ring buffer.
             * Header format (32-bit, little-endian):
             *   bits 0-12:  length of received frame (including 4-byte header)
             *   bit 14:     ROK - receive OK flag
             *   bit 15:     RER - receive error flag */
            uint32_t header = *(uint32_t *)(rx_buffer + read_offset);
            uint16_t pkt_len = header & 0x1FFF;       /* bits 0-12 */
            uint16_t rok     = (header >> 14) & 1;     /* bit 14 */
            uint16_t rer     = (header >> 15) & 1;     /* bit 15 */

            /* Sanity check */
            if (pkt_len == 0 || pkt_len > RTL_RX_BUF_SIZE) {
                break;
            }

            if (rok && !rer) {
                /* Packet data starts 4 bytes after the header, 4-byte aligned.
                 * Data length excludes the 4-byte header. */
                uint32_t data_offset = (read_offset + 4) % RTL_RX_BUF_SIZE;
                uint16_t data_len = pkt_len - 4;

                if (data_offset + data_len <= RTL_RX_BUF_SIZE) {
                    /* No wrap: data is contiguous in the buffer */
                    const uint8_t *frame = rx_buffer + data_offset;
                    /* Ethernet header: 6 dst + 6 src + 2 ethertype = 14 bytes */
                    if (data_len >= 14) {
                        uint16_t ethertype = ((uint16_t)frame[12] << 8) | frame[13];
                        if (ethertype == 0x0800) {
                            /* IPv4 - pass payload (after Ethernet header) to stack */
                            net_rx_ipv4(frame + 14, data_len - 14);
                        } else if (ethertype == 0x0806) {
                            /* ARP - pass payload (after Ethernet header) */
                            net_arp_rx(frame + 14, data_len - 14);
                        }
                    }
                } else {
                    /* Packet wraps around end of ring buffer - reassemble */
                    uint8_t tmp[1792];
                    uint32_t first_part = RTL_RX_BUF_SIZE - data_offset;
                    memcpy(tmp, rx_buffer + data_offset, first_part);
                    memcpy(tmp + first_part, rx_buffer, data_len - first_part);

                    if (data_len >= 14) {
                        uint16_t ethertype = ((uint16_t)tmp[12] << 8) | tmp[13];
                        if (ethertype == 0x0800) {
                            net_rx_ipv4(tmp + 14, data_len - 14);
                        } else if (ethertype == 0x0806) {
                            net_arp_rx(tmp + 14, data_len - 14);
                        }
                    }
                }
            }

            /* Advance the read pointer.
             * new_offset = (read_offset + pkt_len + 4 + 3) & ~3
             * CAPR = new_offset - 16, with acknowledge bit 0x2000 set. */
            uint32_t new_offset = (read_offset + pkt_len + 4 + 3) & ~3;
            uint16_t new_capr = (uint16_t)(new_offset - 16);
            rtl_outw(RTL_REG_CAPR, new_capr | 0x2000);
        }
    }

    /* Handle RX overflow: toggle RX off then on */
    if (status & RTL_INT_RX_OVERFLOW) {
        uint8_t cmd = rtl_inb(RTL_REG_CMD);
        rtl_outb(RTL_REG_CMD, cmd & ~RTL_CMD_RE);
        rtl_outb(RTL_REG_CMD, cmd | RTL_CMD_RE);
    }

    /* TX_OK / TX_ERR: descriptors are polled in rtl8139_send(), nothing extra needed */

    /* Clear handled interrupts by writing 1 to the corresponding ISR bits */
    rtl_outw(RTL_REG_ISR, status);
}
