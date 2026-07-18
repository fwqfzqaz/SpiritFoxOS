#include "pci.h"
#include "hal.h"
#include "vga.h"
#include "string.h"

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static int pci_device_count = 0;

/* ========================================================================
 * PCI 配置空间访问（机制 1）
 * ======================================================================== */

uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uint32_t address = (1UL << 31)
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)dev << 11)
                     | ((uint32_t)func << 8)
                     | ((uint32_t)offset & 0xFC);

    hal_outl(PCI_CONFIG_ADDRESS, address);
    return hal_inl(PCI_CONFIG_DATA);
}

void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value)
{
    uint32_t address = (1UL << 31)
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)dev << 11)
                     | ((uint32_t)func << 8)
                     | ((uint32_t)offset & 0xFC);

    hal_outl(PCI_CONFIG_ADDRESS, address);
    hal_outl(PCI_CONFIG_DATA, value);
}

/* ========================================================================
 * PCI 设备枚举
 * ======================================================================== */

static void pci_scan_function(uint8_t bus, uint8_t dev, uint8_t func)
{
    if (pci_device_count >= PCI_MAX_DEVICES) return;

    uint32_t id = pci_read_config(bus, dev, func, 0x00);
    uint16_t vendor = id & 0xFFFF;
    uint16_t device = (id >> 16) & 0xFFFF;

    /* 无设备存在 */
    if (vendor == 0xFFFF) return;

    pci_device_t *pcidev = &pci_devices[pci_device_count];
    pcidev->bus = bus;
    pcidev->device = dev;
    pcidev->function = func;
    pcidev->vendor_id = vendor;
    pcidev->device_id = device;

    uint32_t class_info = pci_read_config(bus, dev, func, 0x08);
    pcidev->revision    = class_info & 0xFF;
    pcidev->prog_if     = (class_info >> 8) & 0xFF;
    pcidev->subclass    = (class_info >> 16) & 0xFF;
    pcidev->class_code  = (class_info >> 24) & 0xFF;

    uint32_t ht = pci_read_config(bus, dev, func, 0x0C);
    pcidev->header_type = (ht >> 16) & 0xFF;

    /* 读取 BAR（仅适用于类型 0 头部） */
    if ((pcidev->header_type & PCI_HEADER_TYPE_MASK) == 0) {
        for (int i = 0; i < 6; i++) {
            pcidev->bars[i] = pci_read_config(bus, dev, func, 0x10 + i * 4);
        }
    }

    /* 读取中断信息 */
    uint32_t irq_info = pci_read_config(bus, dev, func, 0x3C);
    pcidev->interrupt_line = irq_info & 0xFF;
    pcidev->interrupt_pin  = (irq_info >> 8) & 0xFF;

    pci_device_count++;

    /* 如果是 PCI-to-PCI 桥，扫描次级总线 */
    if (pcidev->class_code == PCI_CLASS_BRIDGE &&
        pcidev->subclass == 0x04) {
        /* 从桥配置中读取次级总线号 */
        uint32_t bus_info = pci_read_config(bus, dev, func, 0x18);
        uint8_t secondary_bus = (bus_info >> 8) & 0xFF;
        if (secondary_bus != 0) {
            /* 扫描次级总线 - 限制深度以防止无限递归 */
            for (int d = 0; d < 32; d++) {
                uint32_t bridged_id = pci_read_config(secondary_bus, d, 0, 0x00);
                if ((bridged_id & 0xFFFF) != 0xFFFF) {
                    pci_scan_function(secondary_bus, d, 0);
                    /* 检查多功能设备 */
                    uint32_t bridged_ht = pci_read_config(secondary_bus, d, 0, 0x0C);
                    if (bridged_ht & (PCI_HEADER_TYPE_MULTI << 16)) {
                        for (int f = 1; f < 8; f++) {
                            bridged_id = pci_read_config(secondary_bus, d, f, 0x00);
                            if ((bridged_id & 0xFFFF) != 0xFFFF) {
                                pci_scan_function(secondary_bus, d, f);
                            }
                        }
                    }
                }
            }
        }
    }
}

static void pci_scan_bus(uint8_t bus)
{
    for (int dev = 0; dev < 32; dev++) {
        uint32_t id = pci_read_config(bus, dev, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF) continue;

        /* 扫描功能 0 */
        pci_scan_function(bus, dev, 0);

        /* 检查是否为多功能设备 */
        uint32_t ht = pci_read_config(bus, dev, 0, 0x0C);
        if ((ht >> 16) & PCI_HEADER_TYPE_MULTI) {
            for (int func = 1; func < 8; func++) {
                uint32_t fid = pci_read_config(bus, dev, func, 0x00);
                if ((fid & 0xFFFF) != 0xFFFF) {
                    pci_scan_function(bus, dev, func);
                }
            }
        }
    }
}

/* ========================================================================
 * PCI 类代码到名称的映射
 * ======================================================================== */

const char* pci_class_name(uint8_t class_code)
{
    switch (class_code) {
    case 0x00: return "Unclassified";
    case 0x01: return "Storage";
    case 0x02: return "Network";
    case 0x03: return "Display";
    case 0x04: return "Multimedia";
    case 0x05: return "Memory";
    case 0x06: return "Bridge";
    case 0x07: return "Comm";
    case 0x08: return "Peripheral";
    case 0x09: return "Input";
    case 0x0A: return "Docking";
    case 0x0B: return "Processor";
    case 0x0C: return "SerialBus";
    case 0x0D: return "Wireless";
    case 0x0E: return "Intelligent";
    case 0x0F: return "Satellite";
    case 0x10: return "Encryption";
    case 0x11: return "SignalProc";
    case 0x12: return "ProcAccel";
    case 0x40: return "Other";
    default:   return "Unknown";
    }
}

/* ========================================================================
 * 公共 API
 * ======================================================================== */

void pci_init(void)
{
    pci_device_count = 0;
    memset(pci_devices, 0, sizeof(pci_devices));

    /* 首先扫描总线 0。如果发现 PCI-to-PCI 桥，
     * 递归扫描其子总线。对于大多数系统这已足够。 */
    pci_scan_bus(0);

    /* 检查是否需要扫描更多总线。
     * 某些实体机（特别是服务器）可能在更高的总线号上有设备，
     * 但主 PCI 主机桥总是在总线 0 上。 */
    printf("[PCI] Found %d device(s)\n", pci_device_count);
}

int pci_get_device_count(void)
{
    return pci_device_count;
}

const pci_device_t* pci_get_device(int index)
{
    if (index < 0 || index >= pci_device_count) return NULL;
    return &pci_devices[index];
}

const pci_device_t* pci_find_device(uint16_t vendor_id, uint16_t device_id)
{
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id &&
            pci_devices[i].device_id == device_id) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

const pci_device_t* pci_find_class(uint8_t class_code)
{
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == class_code) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

void pci_list_devices(void)
{
    printf("PCI Devices (%d found):\n", pci_device_count);
    printf("BUS DEV FN  VENDOR DEVICE  CLASS  SUB  PIF  RECV  NAME\n");
    for (int i = 0; i < pci_device_count; i++) {
        const pci_device_t *d = &pci_devices[i];
        printf("%3d %3d %3d  %04x   %04x   %02x    %02x   %02x   %02x   %s\n",
               d->bus, d->device, d->function,
               d->vendor_id, d->device_id,
               d->class_code, d->subclass, d->prog_if, d->revision,
               pci_class_name(d->class_code));
    }
}
