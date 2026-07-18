#include "ahci.h"
#include "pci.h"
#include "hal.h"
#include "memory.h"
#include "string.h"
#include "blkdev.h"
#include "vga.h"

#define AHCI_SIG_SATA   0x00000101    /* SATA 驱动器 */
#define AHCI_SIG_ATAPI  0xEB140101    /* ATAPI 驱动器 */
#define AHCI_SIG_SEMB   0xC33C0101    /* 箱体管理桥 */
#define AHCI_SIG_PM     0x96690101    /* 端口复用器 */

#define AHCI_PRDT_MAX   8             /* 每个命令的最大 PRD 条目数 */

static ahci_port_t ahci_ports[AHCI_MAX_PORTS];
static int ahci_port_count = 0;

/* ========================================================================
 * AHCI 寄存器 MMIO 辅助函数
 * ======================================================================== */

static inline uint32_t ahci_read(uintptr_t base, uint32_t offset)
{
    return hal_mmio_read32(base + offset);
}

static inline void ahci_write(uintptr_t base, uint32_t offset, uint32_t val)
{
    hal_mmio_write32(base + offset, val);
}

/* ========================================================================
 * 端口操作
 * ======================================================================== */

static int ahci_port_start(ahci_port_t *port)
{
    /* 等待 CR（命令运行）位清零 */
    int timeout = 1000;
    while ((ahci_read(port->port_base, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) && --timeout)
        ;

    /* 设置 ST（启动）和 FRE（FIS 接收使能） */
    uint32_t cmd = ahci_read(port->port_base, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE | AHCI_PORT_CMD_ST;
    ahci_write(port->port_base, AHCI_PORT_CMD, cmd);
    return 0;
}

static int ahci_port_stop(ahci_port_t *port)
{
    /* 清除 ST（启动） */
    uint32_t cmd = ahci_read(port->port_base, AHCI_PORT_CMD);
    cmd &= ~AHCI_PORT_CMD_ST;
    ahci_write(port->port_base, AHCI_PORT_CMD, cmd);

    /* 等待 CR（命令运行）位清零 */
    int timeout = 100000;
    while ((ahci_read(port->port_base, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) && --timeout)
        ;

    /* 清除 FRE（FIS 接收使能） */
    cmd = ahci_read(port->port_base, AHCI_PORT_CMD);
    cmd &= ~AHCI_PORT_CMD_FRE;
    ahci_write(port->port_base, AHCI_PORT_CMD, cmd);

    /* 等待 FR（FIS 运行）位清零 */
    timeout = 100000;
    while ((ahci_read(port->port_base, AHCI_PORT_CMD) & AHCI_PORT_CMD_FR) && --timeout)
        ;

    return 0;
}

static int ahci_port_rebase(ahci_port_t *port)
{
    ahci_port_stop(port);

    /* 清除 SERR（串行错误） */
    ahci_write(port->port_base, AHCI_PORT_SERR, ahci_read(port->port_base, AHCI_PORT_SERR));

    /* 清除 IS（中断状态） */
    ahci_write(port->port_base, AHCI_PORT_IS, ahci_read(port->port_base, AHCI_PORT_IS));

    /* 分配命令列表（必须 1K 对齐，1 页足够） */
    port->cmd_list = (ahci_cmd_header_t *)alloc_page();
    if (!port->cmd_list)
        return -1;
    memset(port->cmd_list, 0, PAGE_SIZE);

    /* 分配 FIS 接收区（必须 256 字节对齐） */
    port->fis_recv = alloc_page();
    if (!port->fis_recv)
        return -1;
    memset(port->fis_recv, 0, PAGE_SIZE);

    /* 分配命令表（必须 128 字节对齐） */
    port->cmd_table = alloc_page();
    if (!port->cmd_table)
        return -1;
    memset(port->cmd_table, 0, PAGE_SIZE);

    /* 设置命令列表基址 */
    uint64_t clb_addr = (uint64_t)(uintptr_t)port->cmd_list;
    ahci_write(port->port_base, AHCI_PORT_CLB, (uint32_t)clb_addr);
    ahci_write(port->port_base, AHCI_PORT_CLBU, (uint32_t)(clb_addr >> 32));

    /* 设置 FIS 基址 */
    uint64_t fb_addr = (uint64_t)(uintptr_t)port->fis_recv;
    ahci_write(port->port_base, AHCI_PORT_FB, (uint32_t)fb_addr);
    ahci_write(port->port_base, AHCI_PORT_FBU, (uint32_t)(fb_addr >> 32));

    /* 为槽位 0 设置命令表 */
    uint64_t ctba_addr = (uint64_t)(uintptr_t)port->cmd_table;
    port->cmd_list[0].ctba = (uint32_t)ctba_addr;
    port->cmd_list[0].ctbau = (uint32_t)(ctba_addr >> 32);

    /* 使能 FIS 接收 */
    uint32_t cmd = ahci_read(port->port_base, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE;
    ahci_write(port->port_base, AHCI_PORT_CMD, cmd);

    /* 上电并启动 */
    cmd |= AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD;
    ahci_write(port->port_base, AHCI_PORT_CMD, cmd);

    /* 启动端口 */
    ahci_port_start(port);

    return 0;
}

/* ========================================================================
 * AHCI 命令执行（PIO 模式）
 * ======================================================================== */

static int ahci_exec_cmd(ahci_port_t *port, int write, uint8_t cmd_byte,
                         uint64_t lba, uint32_t count, void *buf)
{
    /* 等待槽位 0 上之前的命令完成 */
    int timeout = 100000;
    while ((ahci_read(port->port_base, AHCI_PORT_CI) & 0x1) && --timeout)
        ;
    if (timeout <= 0) {
        printf("[AHCI] Previous command still running on port %d\n", port->port_idx);
        return -1;
    }

    /* 清除中断状态 */
    ahci_write(port->port_base, AHCI_PORT_IS, ahci_read(port->port_base, AHCI_PORT_IS));

    /* 设置命令表 FIS（主机到设备寄存器 FIS） */
    uint8_t *fis = (uint8_t *)port->cmd_table;
    memset(fis, 0, 64);  /* 清除 FIS 区域 */

    fis[0] = AHCI_FIS_REG_H2D;         /* FIS 类型 */
    fis[1] = AHCI_FIS_CMD;             /* 命令位 */
    fis[2] = cmd_byte;                  /* ATA 命令 */
    fis[3] = 0;                         /* 特征[7:0] */
    fis[4] = (uint8_t)(lba & 0xFF);           /* LBA 0:7 */
    fis[5] = (uint8_t)((lba >> 8) & 0xFF);    /* LBA 8:15 */
    fis[6] = (uint8_t)((lba >> 16) & 0xFF);   /* LBA 16:23 */
    fis[7] = 0x40 | ((lba >> 24) & 0x0F);    /* 设备：LBA 模式 + LBA 24:27 */
    fis[8] = (uint8_t)((lba >> 32) & 0xFF);   /* LBA 32:39 */
    fis[9] = (uint8_t)((lba >> 40) & 0xFF);   /* LBA 40:47 */
    fis[10] = (uint8_t)((lba >> 48) & 0xFF);  /* LBA 48:55 */
    fis[11] = 0;                               /* 特征[15:8] */
    fis[12] = (uint8_t)(count & 0xFF);        /* 计数[7:0] */
    fis[13] = (uint8_t)((count >> 8) & 0xFF); /* 计数[15:8] */

    /* 在命令表中设置 PRDT（物理区域描述符表） */
    uint64_t buf_addr = (uint64_t)(uintptr_t)buf;
    uint32_t byte_count = count * 512;
    uint32_t remaining = byte_count;
    uint64_t cur_addr = buf_addr;
    int prdt_count = 0;

    ahci_prdt_t *prdt = (ahci_prdt_t *)((uint8_t *)port->cmd_table + 0x80);
    memset(prdt, 0, sizeof(ahci_prdt_t) * AHCI_PRDT_MAX);

    while (remaining > 0 && prdt_count < AHCI_PRDT_MAX) {
        uint32_t chunk = remaining > (4 * 1024 * 1024 - 2) ? (4 * 1024 * 1024 - 2) : remaining;
        chunk = chunk & ~1;  /* 必须为偶数 */

        prdt[prdt_count].dba = (uint32_t)cur_addr;
        prdt[prdt_count].dbau = (uint32_t)(cur_addr >> 32);
        prdt[prdt_count].dbc = chunk - 1;  /* 从 0 开始的计数 */
        prdt[prdt_count].i = 1;            /* 完成时中断 */

        cur_addr += chunk;
        remaining -= chunk;
        prdt_count++;
    }

    /* 为槽位 0 设置命令头 */
    port->cmd_list[0].cfl = 5;    /* 命令 FIS 长度：5 个 DWORD */
    port->cmd_list[0].a = 0;      /* 非 ATAPI */
    port->cmd_list[0].w = write ? 1 : 0;
    port->cmd_list[0].p = 0;
    port->cmd_list[0].prdtl = (uint16_t)prdt_count;
    port->cmd_list[0].prdbc = 0;  /* 清除字节计数 */

    /* 更新命令表基址 */
    uint64_t ctba_addr = (uint64_t)(uintptr_t)port->cmd_table;
    port->cmd_list[0].ctba = (uint32_t)ctba_addr;
    port->cmd_list[0].ctbau = (uint32_t)(ctba_addr >> 32);

    /* 确保端口已启动（ST 和 FRE 应已设置） */
    uint32_t cmd = ahci_read(port->port_base, AHCI_PORT_CMD);
    if (!(cmd & AHCI_PORT_CMD_ST)) {
        /* 端口已停止 - 需要重新启动 */
        /* 首先确保 FRE 已使能 */
        if (!(cmd & AHCI_PORT_CMD_FRE)) {
            cmd |= AHCI_PORT_CMD_FRE;
            ahci_write(port->port_base, AHCI_PORT_CMD, cmd);
            /* 等待 FR 位置位 */
            timeout = 100000;
            while (!((cmd = ahci_read(port->port_base, AHCI_PORT_CMD)) & AHCI_PORT_CMD_FR) && --timeout)
                ;
        }
        /* 然后设置 ST */
        cmd = ahci_read(port->port_base, AHCI_PORT_CMD);
        cmd |= AHCI_PORT_CMD_ST;
        ahci_write(port->port_base, AHCI_PORT_CMD, cmd);
    }

    /* 在槽位 0 上发出命令 */
    ahci_write(port->port_base, AHCI_PORT_CI, 0x1);

    /* 等待命令完成 */
    timeout = 500000;
    while ((ahci_read(port->port_base, AHCI_PORT_CI) & 0x1) && --timeout)
        ;

    if (timeout <= 0) {
        printf("[AHCI] Command timeout on port %d\n", port->port_idx);
        return -1;
    }

    /* 检查错误 */
    uint32_t tfd = ahci_read(port->port_base, AHCI_PORT_TFD);
    if (tfd & 0x01) {  /* 错误位 */
        printf("[AHCI] Command error on port %d, TFD=0x%x\n", port->port_idx, tfd);
        return -2;
    }

    return 0;
}

/* ========================================================================
 * 块设备操作
 * ======================================================================== */

static int ahci_blkdev_read(uint8_t dev_id, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev_id;
    blkdev_t *dev = blkdev_get(dev_id);
    if (!dev || !dev->driver_data)
        return -1;

    ahci_port_t *port = (ahci_port_t *)dev->driver_data;
    return ahci_exec_cmd(port, 0, ATA_READ_DMA_EXT, lba, count, buf);
}

static int ahci_blkdev_write(uint8_t dev_id, uint64_t lba, uint32_t count, const void *buf)
{
    blkdev_t *dev = blkdev_get(dev_id);
    if (!dev || !dev->driver_data)
        return -1;

    ahci_port_t *port = (ahci_port_t *)dev->driver_data;
    return ahci_exec_cmd(port, 1, ATA_WRITE_DMA_EXT, lba, count, (void *)buf);
}

static blkdev_ops_t ahci_blkdev_ops = {
    .read  = ahci_blkdev_read,
    .write = ahci_blkdev_write,
};

/* ========================================================================
 * 从 ATA IDENTIFY 数据获取扇区数
 * ======================================================================== */

static uint64_t ahci_get_sector_count(ahci_port_t *port)
{
    /* 为 IDENTIFY 数据分配临时缓冲区 */
    uint16_t *identify = (uint16_t *)alloc_page();
    if (!identify)
        return 0;

    memset(identify, 0, PAGE_SIZE);

    if (ahci_exec_cmd(port, 0, ATA_IDENTIFY, 0, 1, identify) != 0) {
        free_page(identify);
        return 0;
    }

    /* 检查 LBA48 支持（字 83，位 10） */
    uint64_t sectors = 0;
    if (identify[83] & (1 << 10)) {
        /* LBA48：字 100-103 */
        sectors = (uint64_t)identify[100] |
                  ((uint64_t)identify[101] << 16) |
                  ((uint64_t)identify[102] << 32) |
                  ((uint64_t)identify[103] << 48);
    } else {
        /* LBA28：字 60-61 */
        sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    }

    free_page(identify);
    return sectors;
}

/* ========================================================================
 * 初始化
 * ======================================================================== */

void ahci_init(void)
{
    /* 在 PCI 总线上查找 AHCI 控制器 */
    int dev_count = pci_get_device_count();
    uintptr_t abar = 0;
    int found = 0;

    for (int i = 0; i < dev_count; i++) {
        const pci_device_t *dev = pci_get_device(i);
        if (!dev) continue;

        /* 类 0x01 = 大容量存储，子类 0x06 = SATA，编程接口 0x01 = AHCI */
        if (dev->class_code == 0x01 && dev->subclass == 0x06 && dev->prog_if == 0x01) {
            /* 找到 AHCI 控制器 - 使用 BAR5（ABAR） */
            abar = dev->bars[5] & ~0xF;  /* 清除 I/O 和内存类型位 */

            /* 如果 BAR5 是内存映射的，确保已映射 */
            if (abar) {
                hal_ensure_mapped(abar, 0x1100);  /* 映射足够所有端口使用的空间 */
                found = 1;
                printf("[AHCI] Controller found at PCI %d:%d:%d, ABAR=%p\n",
                       dev->bus, dev->device, dev->function, (void*)abar);
            }
            break;
        }
    }

    if (!found) {
        printf("[AHCI] No AHCI controller found\n");
        return;
    }

    /* 使能 AHCI 模式 */
    uint32_t ghc = ahci_read(abar, AHCI_GHC);
    if (!(ghc & AHCI_GHC_AE)) {
        ahci_write(abar, AHCI_GHC, ghc | AHCI_GHC_AE);
    }

    /* 获取已实现端口位图 */
    uint32_t pi = ahci_read(abar, AHCI_PI);
    uint32_t cap = ahci_read(abar, AHCI_CAP);
    int ncs = (cap >> 8) & 0x1F;  /* 命令槽位数 */

    printf("[AHCI] PI=0x%x, CAP=0x%x, NCS=%d, Version=0x%x\n",
           pi, cap, ncs, ahci_read(abar, 0x10));

    /* 探测每个已实现的端口 */
    for (int i = 0; i < 32 && ahci_port_count < AHCI_MAX_PORTS; i++) {
        if (!(pi & (1 << i)))
            continue;

        uintptr_t port_base = abar + 0x100 + i * 0x80;

        /* 检查设备状态 */
        uint32_t ssts = ahci_read(port_base, AHCI_PORT_SSTS);
        uint32_t det = ssts & AHCI_SSTS_DET_MASK;

        if (det != AHCI_SSTS_DET_PRESENT) {
            continue;  /* 无设备连接 */
        }

        /* 读取设备签名 */
        uint32_t sig = ahci_read(port_base, AHCI_PORT_SIG);

        /* 初始化端口结构 */
        ahci_port_t *port = &ahci_ports[ahci_port_count];
        port->port_idx = i;
        port->mmio_base = abar;
        port->port_base = port_base;
        port->signature = sig;
        port->is_atapi = (sig == AHCI_SIG_ATAPI);

        if (port->is_atapi) {
            printf("[AHCI] Port %d: ATAPI device (signature=0x%x), skipping\n", i, sig);
            continue;
        }

        /* 重置端口（设置命令列表、FIS 等） */
        if (ahci_port_rebase(port) != 0) {
            printf("[AHCI] Port %d: rebase failed\n", i);
            continue;
        }

        /* 获取扇区数 */
        uint64_t sectors = ahci_get_sector_count(port);
        if (sectors == 0) {
            printf("[AHCI] Port %d: IDENTIFY failed, assuming 1MB\n", i);
            sectors = 2048;  /* 回退值：1MB */
        }

        /* 注册为块设备 */
        char name[16];
        name[0] = 's'; name[1] = 'd'; name[2] = 'a' + ahci_port_count; name[3] = '\0';

        port->blkdev_id = blkdev_register(BLKDEV_TYPE_AHCI, sectors,
                                          512, name, &ahci_blkdev_ops, port);

        printf("[AHCI] Port %d: SATA disk %s, %llu sectors (%llu MB)\n",
               i, name, sectors, sectors * 512 / (1024 * 1024));

        ahci_port_count++;
    }

    if (ahci_port_count == 0) {
        printf("[AHCI] No SATA disks found\n");
    }
}
