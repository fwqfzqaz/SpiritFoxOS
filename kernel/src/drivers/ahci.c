#include "ahci.h"
#include "pci.h"
#include "hal.h"
#include "memory.h"
#include "string.h"
#include "blkdev.h"
#include "vga.h"

#define AHCI_SIG_SATA   0x00000101    /* SATA drive */
#define AHCI_SIG_ATAPI  0xEB140101    /* ATAPI drive */
#define AHCI_SIG_SEMB   0xC33C0101    /* Enclosure Management Bridge */
#define AHCI_SIG_PM     0x96690101    /* Port Multiplier */

#define AHCI_PRDT_MAX   8             /* Max PRD entries per command */

static ahci_port_t ahci_ports[AHCI_MAX_PORTS];
static int ahci_port_count = 0;

/* ========================================================================
 * MMIO helpers for AHCI registers
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
 * Port operations
 * ======================================================================== */

static int ahci_port_start(ahci_port_t *port)
{
    /* Wait until CR (Command Running) is cleared */
    int timeout = 1000;
    while ((ahci_read(port->port_base, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) && --timeout)
        ;

    /* Set ST (Start) and FRE (FIS Receive Enable) */
    uint32_t cmd = ahci_read(port->port_base, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE | AHCI_PORT_CMD_ST;
    ahci_write(port->port_base, AHCI_PORT_CMD, cmd);
    return 0;
}

static int ahci_port_stop(ahci_port_t *port)
{
    /* Clear ST (Start) */
    uint32_t cmd = ahci_read(port->port_base, AHCI_PORT_CMD);
    cmd &= ~AHCI_PORT_CMD_ST;
    ahci_write(port->port_base, AHCI_PORT_CMD, cmd);

    /* Wait until CR (Command Running) is cleared */
    int timeout = 100000;
    while ((ahci_read(port->port_base, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) && --timeout)
        ;

    /* Clear FRE (FIS Receive Enable) */
    cmd = ahci_read(port->port_base, AHCI_PORT_CMD);
    cmd &= ~AHCI_PORT_CMD_FRE;
    ahci_write(port->port_base, AHCI_PORT_CMD, cmd);

    /* Wait until FR (FIS Running) is cleared */
    timeout = 100000;
    while ((ahci_read(port->port_base, AHCI_PORT_CMD) & AHCI_PORT_CMD_FR) && --timeout)
        ;

    return 0;
}

static int ahci_port_rebase(ahci_port_t *port)
{
    ahci_port_stop(port);

    /* Clear SERR (Serial Error) */
    ahci_write(port->port_base, AHCI_PORT_SERR, ahci_read(port->port_base, AHCI_PORT_SERR));

    /* Clear IS (Interrupt Status) */
    ahci_write(port->port_base, AHCI_PORT_IS, ahci_read(port->port_base, AHCI_PORT_IS));

    /* Allocate command list (must be 1K aligned, 1 page is enough) */
    port->cmd_list = (ahci_cmd_header_t *)alloc_page();
    if (!port->cmd_list)
        return -1;
    memset(port->cmd_list, 0, PAGE_SIZE);

    /* Allocate FIS receive area (must be 256-byte aligned) */
    port->fis_recv = alloc_page();
    if (!port->fis_recv)
        return -1;
    memset(port->fis_recv, 0, PAGE_SIZE);

    /* Allocate command table (must be 128-byte aligned) */
    port->cmd_table = alloc_page();
    if (!port->cmd_table)
        return -1;
    memset(port->cmd_table, 0, PAGE_SIZE);

    /* Set command list base */
    uint64_t clb_addr = (uint64_t)(uintptr_t)port->cmd_list;
    ahci_write(port->port_base, AHCI_PORT_CLB, (uint32_t)clb_addr);
    ahci_write(port->port_base, AHCI_PORT_CLBU, (uint32_t)(clb_addr >> 32));

    /* Set FIS base */
    uint64_t fb_addr = (uint64_t)(uintptr_t)port->fis_recv;
    ahci_write(port->port_base, AHCI_PORT_FB, (uint32_t)fb_addr);
    ahci_write(port->port_base, AHCI_PORT_FBU, (uint32_t)(fb_addr >> 32));

    /* Set command table for slot 0 */
    uint64_t ctba_addr = (uint64_t)(uintptr_t)port->cmd_table;
    port->cmd_list[0].ctba = (uint32_t)ctba_addr;
    port->cmd_list[0].ctbau = (uint32_t)(ctba_addr >> 32);

    /* Enable FIS receive */
    uint32_t cmd = ahci_read(port->port_base, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE;
    ahci_write(port->port_base, AHCI_PORT_CMD, cmd);

    /* Power on and spin up */
    cmd |= AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD;
    ahci_write(port->port_base, AHCI_PORT_CMD, cmd);

    /* Start the port */
    ahci_port_start(port);

    return 0;
}

/* ========================================================================
 * AHCI command execution (PIO mode)
 * ======================================================================== */

static int ahci_exec_cmd(ahci_port_t *port, int write, uint8_t cmd_byte,
                         uint64_t lba, uint32_t count, void *buf)
{
    /* Wait for any previous command on slot 0 to complete */
    int timeout = 100000;
    while ((ahci_read(port->port_base, AHCI_PORT_CI) & 0x1) && --timeout)
        ;
    if (timeout <= 0) {
        printf("[AHCI] Previous command still running on port %d\n", port->port_idx);
        return -1;
    }

    /* Clear interrupt status */
    ahci_write(port->port_base, AHCI_PORT_IS, ahci_read(port->port_base, AHCI_PORT_IS));

    /* Setup command table FIS (Host-to-Device Register FIS) */
    uint8_t *fis = (uint8_t *)port->cmd_table;
    memset(fis, 0, 64);  /* Clear FIS area */

    fis[0] = AHCI_FIS_REG_H2D;         /* FIS type */
    fis[1] = AHCI_FIS_CMD;             /* Command bit */
    fis[2] = cmd_byte;                  /* ATA command */
    fis[3] = 0;                         /* Features[7:0] */
    fis[4] = (uint8_t)(lba & 0xFF);           /* LBA 0:7 */
    fis[5] = (uint8_t)((lba >> 8) & 0xFF);    /* LBA 8:15 */
    fis[6] = (uint8_t)((lba >> 16) & 0xFF);   /* LBA 16:23 */
    fis[7] = 0x40 | ((lba >> 24) & 0x0F);    /* Device: LBA mode + LBA 24:27 */
    fis[8] = (uint8_t)((lba >> 32) & 0xFF);   /* LBA 32:39 */
    fis[9] = (uint8_t)((lba >> 40) & 0xFF);   /* LBA 40:47 */
    fis[10] = (uint8_t)((lba >> 48) & 0xFF);  /* LBA 48:55 */
    fis[11] = 0;                               /* Features[15:8] */
    fis[12] = (uint8_t)(count & 0xFF);        /* Count[7:0] */
    fis[13] = (uint8_t)((count >> 8) & 0xFF); /* Count[15:8] */

    /* Setup PRDT (Physical Region Descriptor Table) in command table */
    uint64_t buf_addr = (uint64_t)(uintptr_t)buf;
    uint32_t byte_count = count * 512;
    uint32_t remaining = byte_count;
    uint64_t cur_addr = buf_addr;
    int prdt_count = 0;

    ahci_prdt_t *prdt = (ahci_prdt_t *)((uint8_t *)port->cmd_table + 0x80);
    memset(prdt, 0, sizeof(ahci_prdt_t) * AHCI_PRDT_MAX);

    while (remaining > 0 && prdt_count < AHCI_PRDT_MAX) {
        uint32_t chunk = remaining > (4 * 1024 * 1024 - 2) ? (4 * 1024 * 1024 - 2) : remaining;
        chunk = chunk & ~1;  /* Must be even number */

        prdt[prdt_count].dba = (uint32_t)cur_addr;
        prdt[prdt_count].dbau = (uint32_t)(cur_addr >> 32);
        prdt[prdt_count].dbc = chunk - 1;  /* 0-based count */
        prdt[prdt_count].i = 1;            /* Interrupt on completion */

        cur_addr += chunk;
        remaining -= chunk;
        prdt_count++;
    }

    /* Setup command header for slot 0 */
    port->cmd_list[0].cfl = 5;    /* Command FIS length: 5 DWORDs */
    port->cmd_list[0].a = 0;      /* Not ATAPI */
    port->cmd_list[0].w = write ? 1 : 0;
    port->cmd_list[0].p = 0;
    port->cmd_list[0].prdtl = (uint16_t)prdt_count;
    port->cmd_list[0].prdbc = 0;  /* Clear byte count */

    /* Update command table base */
    uint64_t ctba_addr = (uint64_t)(uintptr_t)port->cmd_table;
    port->cmd_list[0].ctba = (uint32_t)ctba_addr;
    port->cmd_list[0].ctbau = (uint32_t)(ctba_addr >> 32);

    /* Ensure the port is started (ST and FRE should be set) */
    uint32_t cmd = ahci_read(port->port_base, AHCI_PORT_CMD);
    if (!(cmd & AHCI_PORT_CMD_ST)) {
        /* Port was stopped - need to restart it */
        /* First make sure FRE is enabled */
        if (!(cmd & AHCI_PORT_CMD_FRE)) {
            cmd |= AHCI_PORT_CMD_FRE;
            ahci_write(port->port_base, AHCI_PORT_CMD, cmd);
            /* Wait for FR to be set */
            timeout = 100000;
            while (!((cmd = ahci_read(port->port_base, AHCI_PORT_CMD)) & AHCI_PORT_CMD_FR) && --timeout)
                ;
        }
        /* Now set ST */
        cmd = ahci_read(port->port_base, AHCI_PORT_CMD);
        cmd |= AHCI_PORT_CMD_ST;
        ahci_write(port->port_base, AHCI_PORT_CMD, cmd);
    }

    /* Issue command on slot 0 */
    ahci_write(port->port_base, AHCI_PORT_CI, 0x1);

    /* Wait for command completion */
    timeout = 500000;
    while ((ahci_read(port->port_base, AHCI_PORT_CI) & 0x1) && --timeout)
        ;

    if (timeout <= 0) {
        printf("[AHCI] Command timeout on port %d\n", port->port_idx);
        return -1;
    }

    /* Check for errors */
    uint32_t tfd = ahci_read(port->port_base, AHCI_PORT_TFD);
    if (tfd & 0x01) {  /* Error bit */
        printf("[AHCI] Command error on port %d, TFD=0x%x\n", port->port_idx, tfd);
        return -2;
    }

    return 0;
}

/* ========================================================================
 * Block device operations
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
 * Get sector count from ATA IDENTIFY data
 * ======================================================================== */

static uint64_t ahci_get_sector_count(ahci_port_t *port)
{
    /* Allocate a temporary buffer for IDENTIFY data */
    uint16_t *identify = (uint16_t *)alloc_page();
    if (!identify)
        return 0;

    memset(identify, 0, PAGE_SIZE);

    if (ahci_exec_cmd(port, 0, ATA_IDENTIFY, 0, 1, identify) != 0) {
        free_page(identify);
        return 0;
    }

    /* Check LBA48 support (word 83, bit 10) */
    uint64_t sectors = 0;
    if (identify[83] & (1 << 10)) {
        /* LBA48: words 100-103 */
        sectors = (uint64_t)identify[100] |
                  ((uint64_t)identify[101] << 16) |
                  ((uint64_t)identify[102] << 32) |
                  ((uint64_t)identify[103] << 48);
    } else {
        /* LBA28: words 60-61 */
        sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    }

    free_page(identify);
    return sectors;
}

/* ========================================================================
 * Initialization
 * ======================================================================== */

void ahci_init(void)
{
    /* Find AHCI controller on PCI bus */
    int dev_count = pci_get_device_count();
    uintptr_t abar = 0;
    int found = 0;

    for (int i = 0; i < dev_count; i++) {
        const pci_device_t *dev = pci_get_device(i);
        if (!dev) continue;

        /* Class 0x01 = Mass Storage, Subclass 0x06 = SATA, Prog IF 0x01 = AHCI */
        if (dev->class_code == 0x01 && dev->subclass == 0x06 && dev->prog_if == 0x01) {
            /* Found AHCI controller - use BAR5 (ABAR) */
            abar = dev->bars[5] & ~0xF;  /* Clear I/O and memory type bits */

            /* If BAR5 is memory-mapped, ensure it's mapped */
            if (abar) {
                hal_ensure_mapped(abar, 0x1100);  /* Map enough for all ports */
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

    /* Enable AHCI mode */
    uint32_t ghc = ahci_read(abar, AHCI_GHC);
    if (!(ghc & AHCI_GHC_AE)) {
        ahci_write(abar, AHCI_GHC, ghc | AHCI_GHC_AE);
    }

    /* Get ports implemented bitmap */
    uint32_t pi = ahci_read(abar, AHCI_PI);
    uint32_t cap = ahci_read(abar, AHCI_CAP);
    int ncs = (cap >> 8) & 0x1F;  /* Number of command slots */

    printf("[AHCI] PI=0x%x, CAP=0x%x, NCS=%d, Version=0x%x\n",
           pi, cap, ncs, ahci_read(abar, 0x10));

    /* Probe each implemented port */
    for (int i = 0; i < 32 && ahci_port_count < AHCI_MAX_PORTS; i++) {
        if (!(pi & (1 << i)))
            continue;

        uintptr_t port_base = abar + 0x100 + i * 0x80;

        /* Check device status */
        uint32_t ssts = ahci_read(port_base, AHCI_PORT_SSTS);
        uint32_t det = ssts & AHCI_SSTS_DET_MASK;

        if (det != AHCI_SSTS_DET_PRESENT) {
            continue;  /* No device connected */
        }

        /* Read device signature */
        uint32_t sig = ahci_read(port_base, AHCI_PORT_SIG);

        /* Initialize port structure */
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

        /* Rebase port (set up command list, FIS, etc.) */
        if (ahci_port_rebase(port) != 0) {
            printf("[AHCI] Port %d: rebase failed\n", i);
            continue;
        }

        /* Get sector count */
        uint64_t sectors = ahci_get_sector_count(port);
        if (sectors == 0) {
            printf("[AHCI] Port %d: IDENTIFY failed, assuming 1MB\n", i);
            sectors = 2048;  /* Fallback: 1MB */
        }

        /* Register as block device */
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
