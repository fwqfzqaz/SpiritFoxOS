#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

/* AHCI register offsets from ABAR base */
#define AHCI_GHC        0x04    /* Global HBA Control */
#define AHCI_IS         0x10    /* Interrupt Status */
#define AHCI_PI         0x0C    /* Ports Implemented */
#define AHCI_VS         0x10    /* Version */
#define AHCI_CCC_CTL    0x14    /* Command Completion Coalescing Control */
#define AHCI_CCC_PORTS  0x1C    /* CCC Ports */
#define AHCI_CAP        0x00    /* HBA Capabilities */
#define AHCI_CAP2       0x24    /* HBA Capabilities Extended */

/* Port register offsets (0x100 + 0x80 * port) */
#define AHCI_PORT_CLB       0x00    /* Command List Base */
#define AHCI_PORT_CLBU      0x04    /* Command List Base Upper */
#define AHCI_PORT_FB        0x08    /* FIS Base */
#define AHCI_PORT_FBU       0x0C    /* FIS Base Upper */
#define AHCI_PORT_IS        0x10    /* Interrupt Status */
#define AHCI_PORT_IE        0x14    /* Interrupt Enable */
#define AHCI_PORT_CMD       0x18    /* Command and Status */
#define AHCI_PORT_TFD       0x20    /* Task File Data */
#define AHCI_PORT_SIG       0x24    /* Signature */
#define AHCI_PORT_SSTS      0x28    /* Serial Status */
#define AHCI_PORT_SCTL      0x2C    /* Serial Control */
#define AHCI_PORT_SERR      0x30    /* Serial Error */
#define AHCI_PORT_SACT      0x34    /* Serial Active */
#define AHCI_PORT_CI        0x38    /* Command Issue */
#define AHCI_PORT_SNTF      0x3C    /* Serial Notification */

/* GHC flags */
#define AHCI_GHC_AE         (1 << 31)  /* AHCI Enable */
#define AHCI_GHC_MRSM       (1 << 2)   /* MSI Revert to Single Message */
#define AHCI_GHC_IE         (1 << 1)   /* Interrupt Enable */
#define AHCI_GHC_HR         (1 << 0)   /* HBA Reset */

/* CMD flags */
#define AHCI_PORT_CMD_ICC   (0xF << 28)  /* Interface Communication Control */
#define AHCI_PORT_CMD_ASP   (1 << 27)     /* Aggressive Slumber/Partial */
#define AHCI_PORT_CMD_ALPE  (1 << 26)     /* Aggressive Link PM Enable */
#define AHCI_PORT_CMD_DLAE  (1 << 25)     /* Drive LED on ATAPI Enable */
#define AHCI_PORT_CMD_ATAPI (1 << 24)     /* Device is ATAPI */
#define AHCI_PORT_CMD_CPD   (1 << 20)     /* Cold Presence Detection */
#define AHCI_PORT_CMD_MPSP  (1 << 19)     /* Mechanical Presence Switch */
#define AHCI_PORT_CMD_HPCP  (1 << 18)     /* Hot Plug Capable Port */
#define AHCI_PORT_CMD_PMA   (1 << 17)     /* Port Multiplier Attached */
#define AHCI_PORT_CMD_CPS   (1 << 16)     /* Cold Presence State */
#define AHCI_PORT_CMD_CR    (1 << 15)     /* Command Running */
#define AHCI_PORT_CMD_FR    (1 << 14)     /* FIS Running */
#define AHCI_PORT_CMD_MPSS  (1 << 13)     /* Mechanical Presence Switch State */
#define AHCI_PORT_CMD_CCS   0x1F          /* Current Command Slot */
#define AHCI_PORT_CMD_FRE   (1 << 4)      /* FIS Receive Enable */
#define AHCI_PORT_CMD_CLO   (1 << 3)      /* Command List Override */
#define AHCI_PORT_CMD_POD   (1 << 2)      /* Power On Device */
#define AHCI_PORT_CMD_SUD   (1 << 1)      /* Start Up Device */
#define AHCI_PORT_CMD_ST    (1 << 0)      /* Start */

/* SSTS values */
#define AHCI_SSTS_DET_MASK  0xF
#define AHCI_SSTS_DET_PRESENT 3  /* Device present and communication established */

/* Command header flags */
#define AHCI_CMDH_CFL      0x1F         /* Command FIS Length (in DWORDs) */
#define AHCI_CMDH_A        (1 << 5)     /* ATAPI */
#define AHCI_CMDH_W        (1 << 6)     /* Write */
#define AHCI_CMDH_P        (1 << 7)     /* Prefetchable */
#define AHCI_CMDH_R        (1 << 8)     /* Reset */
#define AHCI_CMDH_B        (1 << 9)     /* BIST */
#define AHCI_CMDH_C       (1 << 10)     /* Clear Busy on R_OK */

/* FIS types */
#define AHCI_FIS_REG_H2D   0x27    /* Register - Host to Device */
#define AHCI_FIS_REG_D2H   0x34    /* Register - Device to Host */

/* Command FIS flags */
#define AHCI_FIS_CMD       (1 << 7)  /* Command bit */

/* ATA commands */
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_IDENTIFY       0xEC

#define AHCI_MAX_PORTS     32
#define AHCI_CMD_SLOTS     32
#define AHCI_CMD_TBL_SIZE  128

/* AHCI Command Header */
typedef struct {
    uint8_t  cfl:5;     /* Command FIS Length */
    uint8_t  a:1;       /* ATAPI */
    uint8_t  w:1;       /* Write */
    uint8_t  p:1;       /* Prefetchable */
    uint8_t  r:1;       /* Reset */
    uint8_t  b:1;       /* BIST */
    uint8_t  c:1;       /* Clear Busy */
    uint8_t  res1:1;
    uint8_t  pmp:4;     /* Port Multiplier Port */
    uint16_t prdtl;     /* Physical Region Descriptor Table Length */
    uint32_t prdbc;     /* Physical Region Descriptor Byte Count */
    uint32_t ctba;      /* Command Table Base Address */
    uint32_t ctbau;     /* Command Table Base Address Upper 32 */
    uint32_t res2[4];
} __attribute__((packed)) ahci_cmd_header_t;

/* AHCI Physical Region Descriptor */
typedef struct {
    uint32_t dba;       /* Data Base Address */
    uint32_t dbau;      /* Data Base Address Upper 32 */
    uint32_t res1;
    uint32_t dbc:22;    /* Data Byte Count */
    uint32_t res2:9;
    uint32_t i:1;       /* Interrupt on Completion */
} __attribute__((packed)) ahci_prdt_t;

/* AHCI port private data */
typedef struct {
    uint32_t    port_idx;       /* Port index */
    uintptr_t   mmio_base;      /* ABAR base address */
    uintptr_t   port_base;      /* Port register base */
    uint32_t    signature;      /* Device signature */
    int         is_atapi;       /* Is ATAPI device */
    int         blkdev_id;      /* Block device ID */

    /* Allocated memory for command structures */
    ahci_cmd_header_t *cmd_list;   /* Command list (1 page) */
    void              *cmd_table;  /* Command table (1 page, aligned) */
    void              *fis_recv;   /* FIS receive area (256 bytes, aligned) */
} ahci_port_t;

/* Initialize AHCI driver - scans PCI bus for AHCI controllers */
void ahci_init(void);

#endif /* AHCI_H */
