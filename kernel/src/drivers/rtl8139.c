/*
 * rtl8139.c - SpiritFoxOS 的 RTL8139 以太网驱动
 *
 * Realtek RTL8139 快速以太网控制器驱动，
 * 该控制器是 QEMU 的默认网卡（-net nic）。
 * 使用 PIO（端口 I/O）模式通过 BAR0 访问。
 *
 * 寄存器布局参考 Realtek RTL8139C(L) 数据手册 / Linux 8139too.c：
 *   0x00  MAC0-5        (6 字节, 只读)   以太网硬件地址
 *   0x08  MAR0-7        (8 字节, 读写)   多播哈希
 *   0x10  TSD0-TSD3     (4x4 字节, 读写) 发送状态描述符
 *   0x20  TSAD0-TSAD3   (4x4 字节, 读写) 发送起始地址描述符
 *   0x30  RBSTART       (4 字节, 读写)   接收缓冲区起始地址
 *   0x37  ChipCmd       (1 字节, 读写)   命令寄存器
 *   0x38  RxBufPtr      (2 字节, 读写)   当前包读取地址 (CAPR)
 *   0x3A  RxBufAddr     (2 字节, 只读)   当前缓冲区地址 (CBR)
 *   0x3C  IntrMask      (2 字节, 读写)   中断屏蔽
 *   0x3E  IntrStatus    (2 字节, 读写)   中断状态
 *   0x40  TxConfig      (4 字节, 读写)   发送配置
 *   0x44  RxConfig      (4 字节, 读写)   接收配置
 *   0x52  Config1       (1 字节, 读写)   配置寄存器 1
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
 * RTL8139 PCI 标识
 * ======================================================================== */
#define RTL8139_VENDOR_ID  0x10EC
#define RTL8139_DEVICE_ID  0x8139

/* ========================================================================
 * RTL8139 I/O 寄存器偏移（相对于 BAR0 基址）
 * ======================================================================== */
#define RTL_REG_MAC0       0x00    /* MAC 地址字节 0-5（只读） */
#define RTL_REG_MAR0       0x08    /* 多播哈希（8 字节，读写） */
#define RTL_REG_TSD0       0x10    /* 发送状态描述符 0（32 位，读写） */
#define RTL_REG_TSD1       0x14    /* 发送状态描述符 1 */
#define RTL_REG_TSD2       0x18    /* 发送状态描述符 2 */
#define RTL_REG_TSD3       0x1C    /* 发送状态描述符 3 */
#define RTL_REG_TSAD0      0x20    /* 发送起始地址 0（32 位，读写） */
#define RTL_REG_TSAD1      0x24    /* 发送起始地址 1 */
#define RTL_REG_TSAD2      0x28    /* 发送起始地址 2 */
#define RTL_REG_TSAD3      0x2C    /* 发送起始地址 3 */
#define RTL_REG_RBSTART    0x30    /* 接收缓冲区起始地址（32 位，读写） */
#define RTL_REG_CMD        0x37    /* 命令寄存器（8 位，读写） */
#define RTL_REG_CAPR       0x38    /* 当前包读取地址（16 位，读写） */
#define RTL_REG_CBR        0x3A    /* 当前缓冲区地址（16 位，只读） */
#define RTL_REG_IMR        0x3C    /* 中断屏蔽（16 位，读写） */
#define RTL_REG_ISR        0x3E    /* 中断状态（16 位，读写） */
#define RTL_REG_TXCFG      0x40    /* 发送配置（32 位，读写） */
#define RTL_REG_RXCFG      0x44    /* 接收配置（32 位，读写） */
#define RTL_REG_CONFIG1    0x52    /* 配置寄存器 1（8 位，读写） */

/* ========================================================================
 * ChipCmd (0x37) 位定义
 * ======================================================================== */
#define RTL_CMD_BUFE       (1 << 0)   /* 接收缓冲区空（只读） */
#define RTL_CMD_RE         (1 << 2)   /* 接收器使能 */
#define RTL_CMD_TE         (1 << 3)   /* 发送器使能 */
#define RTL_CMD_RST        (1 << 4)   /* 软件复位 */

/* ========================================================================
 * ISR / IMR 位定义 (0x3E / 0x3C)
 * ======================================================================== */
#define RTL_INT_RX_OK      (1 << 0)   /* 包接收成功 */
#define RTL_INT_RX_ERR     (1 << 1)   /* 接收错误 */
#define RTL_INT_TX_OK      (1 << 2)   /* 包发送成功 */
#define RTL_INT_TX_ERR     (1 << 3)   /* 发送错误 */
#define RTL_INT_RX_FIFO    (1 << 4)   /* 接收 FIFO 溢出 */
#define RTL_INT_LINK       (1 << 5)   /* 链路变化 */
#define RTL_INT_RX_UNDERRUN (1 << 6)  /* 接收欠载 */
#define RTL_INT_RX_OVERFLOW (1 << 7)  /* 接收缓冲区溢出 */
#define RTL_INT_TIMEOUT    (1 << 14)  /* 线缆长度 / 超时 */
#define RTL_INT_SYSERR     (1 << 15)  /* 系统错误 */

/* ========================================================================
 * TSD（发送状态描述符）位定义
 * ======================================================================== */
#define RTL_TSD_OWN        (1 << 13)  /* OWN：置 1 启动发送，硬件完成后清零 */
#define RTL_TSD_TOK        (1 << 15)  /* 发送成功 */
#define RTL_TSD_TUN        (1 << 14)  /* 发送 FIFO 欠载 */
#define RTL_TSD_TABT       (1 << 18)  /* 发送中止 */

/* ========================================================================
 * 接收配置
 * ======================================================================== */
/* 接受广播 + 多播 + 物理匹配 + 完美匹配，
 * 最大 DMA 突发（0xF << 8），回绕模式 */
#define RTL_RXCFG_VAL     0x00000F0E

/* ========================================================================
 * 发送配置
 * ======================================================================== */
/* 标准帧间隔（0x3 << 24），最大 DMA 突发 256 字节 */
#define RTL_TXCFG_VAL     0x03000000

/* ========================================================================
 * 接收环形缓冲区常量
 * ======================================================================== */
#define RTL_RX_BUF_SIZE   8192       /* 8K 环形缓冲区 */
#define RTL_RX_BUF_PAD    16         /* 16 字节回绕填充 */
#define RTL_RX_BUF_EXTRA  1500       /* 回绕最大帧的额外空间 */
#define RTL_RX_BUF_TOTAL  (RTL_RX_BUF_SIZE + RTL_RX_BUF_PAD + RTL_RX_BUF_EXTRA)

/* 发送缓冲区大小（每个描述符） */
#define RTL_TX_BUF_SIZE   1792

/* 发送描述符数量 */
#define RTL_TX_DESCRIPTORS 4

/* ========================================================================
 * 驱动状态
 * ======================================================================== */
static uint16_t  rtl_io_base = 0;
static uint8_t   rtl_irq = 0;
static uint8_t   rtl_mac[6];
static uint8_t  *rx_buffer = NULL;
static uint8_t  *tx_buffers[RTL_TX_DESCRIPTORS] = {NULL};
static uint32_t  tx_current = 0;
static int       rtl_initialized = 0;

/* ========================================================================
 * I/O 辅助封装
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
 * PCI 设备发现
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
 * 公共 API
 * ======================================================================== */

void rtl8139_init(void)
{
    /* 步骤 1：在 PCI 总线上查找 RTL8139 */
    const pci_device_t *pcidev = rtl8139_find_pci();
    if (!pcidev) {
        printf("[RTL8139] Device not found on PCI bus\n");
        return;
    }

    printf("[RTL8139] Found at PCI %02x:%02x.%x\n",
           pcidev->bus, pcidev->device, pcidev->function);

    /* 步骤 2：使能 PCI 总线主控和 I/O 空间访问 */
    uint32_t cmd = pci_read_config(pcidev->bus, pcidev->device,
                                    pcidev->function, 0x04);
    pci_write_config(pcidev->bus, pcidev->device, pcidev->function,
                     0x04, cmd | 0x05);  /* I/O 空间使能 + 总线主控使能 */

    /* 步骤 3：读取 BAR0 获取 I/O 基地址 */
    uint32_t bar0 = pci_read_config(pcidev->bus, pcidev->device,
                                     pcidev->function, 0x10);
    if (!(bar0 & 0x01)) {
        printf("[RTL8139] BAR0 is not I/O space (bar0=0x%x)\n", bar0);
        return;
    }
    rtl_io_base = (uint16_t)(bar0 & ~0x03);  /* 屏蔽低 2 位 */
    printf("[RTL8139] I/O base: 0x%x\n", rtl_io_base);

    /* 步骤 4：读取 IRQ 线 */
    rtl_irq = pcidev->interrupt_line;
    if (rtl_irq == 0 || rtl_irq == 0xFF) {
        printf("[RTL8139] No IRQ assigned, defaulting to IRQ 11\n");
        rtl_irq = 11;
    }
    printf("[RTL8139] IRQ: %u\n", rtl_irq);

    /* 步骤 5：上电网卡（清除 CONFIG1 中的 LWACT+LWPTN 位） */
    rtl_outb(RTL_REG_CONFIG1, 0x00);

    /* 步骤 6：软件复位 */
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

    /* 步骤 7：读取 MAC 地址（0x00 处的 6 字节） */
    for (int i = 0; i < 6; i++) {
        rtl_mac[i] = rtl_inb(RTL_REG_MAC0 + i);
    }
    printf("[RTL8139] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           rtl_mac[0], rtl_mac[1], rtl_mac[2],
           rtl_mac[3], rtl_mac[4], rtl_mac[5]);

    /* 步骤 8：分配接收缓冲区（必须 16 字节对齐）。
     * 内核使用恒等映射，因此虚拟地址 == 物理地址。
     * 3 页 = 12288 字节 >= 8192 + 16 + 1500 = 9708 字节。 */
    rx_buffer = (uint8_t *)alloc_pages(3);
    if (!rx_buffer) {
        printf("[RTL8139] Failed to allocate RX buffer\n");
        return;
    }
    memset(rx_buffer, 0, RTL_RX_BUF_TOTAL);

    /* 将接收缓冲区的物理地址写入 RBSTART */
    rtl_outl(RTL_REG_RBSTART, (uint32_t)(uintptr_t)rx_buffer);

    /* 步骤 9：分配发送缓冲区（4 个描述符，各一页） */
    for (int i = 0; i < RTL_TX_DESCRIPTORS; i++) {
        tx_buffers[i] = (uint8_t *)alloc_page();
        if (!tx_buffers[i]) {
            printf("[RTL8139] Failed to allocate TX buffer %d\n", i);
            return;
        }
    }

    /* 步骤 10：设置发送配置 */
    rtl_outl(RTL_REG_TXCFG, RTL_TXCFG_VAL);

    /* 步骤 11：设置接收配置（接受广播 + 多播 +
     * 物理匹配 + 完美匹配，最大 DMA 突发，回绕模式） */
    rtl_outl(RTL_REG_RXCFG, RTL_RXCFG_VAL);

    /* 步骤 12：清除所有待处理中断 */
    rtl_outw(RTL_REG_ISR, 0xFFFF);

    /* 步骤 13：使能中断：RX_OK | TX_OK | RX_ERR | TX_ERR | RX_OVERFLOW */
    rtl_outw(RTL_REG_IMR, RTL_INT_RX_OK | RTL_INT_TX_OK |
                          RTL_INT_RX_ERR | RTL_INT_TX_ERR |
                          RTL_INT_RX_OVERFLOW);

    /* 步骤 14：使能接收器和发送器 */
    rtl_outb(RTL_REG_CMD, RTL_CMD_RE | RTL_CMD_TE);

    /* 步骤 15：通过 IOAPIC 路由 IRQ */
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

    /* 限制为最大发送帧大小 */
    if (len > RTL_TX_BUF_SIZE)
        len = RTL_TX_BUF_SIZE;

    /* 查找空闲的发送描述符（硬件发送完成后清除 OWN 位） */
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
        /* 所有描述符忙，尝试推进 */
        tx_current = (tx_current + 1) % RTL_TX_DESCRIPTORS;
        tsd = rtl_inl(RTL_REG_TSD0 + tx_current * 4);
        if (tsd & RTL_TSD_OWN) {
            printf("[RTL8139] TX timeout - all descriptors busy\n");
            return;
        }
    }

    /* 将包数据复制到发送缓冲区 */
    memcpy(tx_buffers[tx_current], data, len);

    /* 写入发送起始地址 */
    rtl_outl(RTL_REG_TSAD0 + tx_current * 4,
             (uint32_t)(uintptr_t)tx_buffers[tx_current]);

    /* 写入发送状态描述符：大小 | OWN 位以触发发送 */
    rtl_outl(RTL_REG_TSD0 + tx_current * 4, (uint32_t)len | RTL_TSD_OWN);

    /* 推进到下一个描述符 */
    tx_current = (tx_current + 1) % RTL_TX_DESCRIPTORS;
}

void rtl8139_irq_handler(void)
{
    if (!rtl_initialized)
        return;

    /* 读取中断状态 */
    uint16_t status = rtl_inw(RTL_REG_ISR);

    if (status == 0) {
        /* 非本设备中断 */
        return;
    }

    /* 处理接收：处理环形缓冲区中所有可用的包 */
    if (status & (RTL_INT_RX_OK | RTL_INT_RX_ERR | RTL_INT_RX_OVERFLOW)) {
        while (!(rtl_inb(RTL_REG_CMD) & RTL_CMD_BUFE)) {
            /* CAPR 存储的是（当前读取位置 - 0x10）。
             * 复位后 CAPR = 0xFFF0，因此初始读取偏移 = 0xFFF0 + 16 = 0x10000 = 0 mod 8K。 */
            uint16_t capr = rtl_inw(RTL_REG_CAPR);
            uint32_t read_offset = (uint32_t)(capr + 16) % RTL_RX_BUF_SIZE;

            /* 从环形缓冲区读取 4 字节包头。
             * 头格式（32 位，小端序）：
             *   位 0-12：  接收帧长度（含 4 字节头）
             *   位 14：    ROK - 接收成功标志
             *   位 15：    RER - 接收错误标志 */
            uint32_t header = *(uint32_t *)(rx_buffer + read_offset);
            uint16_t pkt_len = header & 0x1FFF;       /* 位 0-12 */
            uint16_t rok     = (header >> 14) & 1;     /* 位 14 */
            uint16_t rer     = (header >> 15) & 1;     /* 位 15 */

            /* 合理性检查 */
            if (pkt_len == 0 || pkt_len > RTL_RX_BUF_SIZE) {
                break;
            }

            if (rok && !rer) {
                /* 包数据在头之后 4 字节处开始，4 字节对齐。
                 * 数据长度不包含 4 字节头。 */
                uint32_t data_offset = (read_offset + 4) % RTL_RX_BUF_SIZE;
                uint16_t data_len = pkt_len - 4;

                if (data_offset + data_len <= RTL_RX_BUF_SIZE) {
                    /* 无回绕：数据在缓冲区中连续 */
                    const uint8_t *frame = rx_buffer + data_offset;
                    /* 以太网头：6 目标 + 6 源 + 2 以太类型 = 14 字节 */
                    if (data_len >= 14) {
                        uint16_t ethertype = ((uint16_t)frame[12] << 8) | frame[13];
                        if (ethertype == 0x0800) {
                            /* IPv4 - 将有效载荷（以太网头之后）传递给协议栈 */
                            net_rx_ipv4(frame + 14, data_len - 14);
                        } else if (ethertype == 0x0806) {
                            /* ARP - 将有效载荷（以太网头之后）传递 */
                            net_arp_rx(frame + 14, data_len - 14);
                        }
                    }
                } else {
                    /* 包跨越环形缓冲区末尾回绕 - 重组 */
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

            /* 推进读取指针。
             * new_offset = (read_offset + pkt_len + 4 + 3) & ~3
             * CAPR = new_offset - 16，并设置确认位 0x2000。 */
            uint32_t new_offset = (read_offset + pkt_len + 4 + 3) & ~3;
            uint16_t new_capr = (uint16_t)(new_offset - 16);
            rtl_outw(RTL_REG_CAPR, new_capr | 0x2000);
        }
    }

    /* 处理接收溢出：关闭再打开接收器 */
    if (status & RTL_INT_RX_OVERFLOW) {
        uint8_t cmd = rtl_inb(RTL_REG_CMD);
        rtl_outb(RTL_REG_CMD, cmd & ~RTL_CMD_RE);
        rtl_outb(RTL_REG_CMD, cmd | RTL_CMD_RE);
    }

    /* TX_OK / TX_ERR：描述符在 rtl8139_send() 中轮询，无需额外处理 */

    /* 通过向对应的 ISR 位写入 1 来清除已处理的中断 */
    rtl_outw(RTL_REG_ISR, status);
}
