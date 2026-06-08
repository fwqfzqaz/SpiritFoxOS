#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "vga.h"
#include "../include/io.h"
#include "../include/string.h"

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static int pci_device_count = 0;

uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t address = ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                       ((uint32_t)func << 8) | (offset & 0xFC) | 0x80000000;
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                       ((uint32_t)func << 8) | (offset & 0xFC) | 0x80000000;
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static uint64_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t func, int bar_index) {
    uint8_t offset = 0x10 + bar_index * 4;
    uint32_t bar_low = pci_read_config(bus, dev, func, offset);

    if ((bar_low & 0x01) == 0) {
        if ((bar_low & 0x06) == 0x04) {
            uint32_t bar_high = pci_read_config(bus, dev, func, offset + 4);
            return ((uint64_t)bar_high << 32) | (bar_low & 0xFFFFFFF0);
        }
        return bar_low & 0xFFFFFFF0;
    }
    return bar_low & 0xFFFFFFFC;
}

static uint32_t pci_get_bar_size(uint8_t bus, uint8_t dev, uint8_t func, int bar_index) {
    uint8_t offset = 0x10 + bar_index * 4;
    uint32_t orig = pci_read_config(bus, dev, func, offset);

    pci_write_config(bus, dev, func, offset, 0xFFFFFFFF);
    uint32_t size_val = pci_read_config(bus, dev, func, offset);
    pci_write_config(bus, dev, func, offset, orig);

    if ((orig & 0x01) == 0) {
        if ((orig & 0x06) == 0x04) {
            uint32_t orig_high = pci_read_config(bus, dev, func, offset + 4);
            pci_write_config(bus, dev, func, offset + 4, 0xFFFFFFFF);
            uint32_t size_high = pci_read_config(bus, dev, func, offset + 4);
            pci_write_config(bus, dev, func, offset + 4, orig_high);

            uint64_t size64 = (((uint64_t)size_high << 32) | (size_val & 0xFFFFFFF0));
            size64 = ~size64 + 1;
            return (uint32_t)size64;
        }
        return ~(size_val & 0xFFFFFFF0) + 1;
    }
    return ~(size_val & 0xFFFFFFFC) + 1;
}

static void pci_scan_function(uint8_t bus, uint8_t dev, uint8_t func) {
    if (pci_device_count >= PCI_MAX_DEVICES) return;

    uint32_t id = pci_read_config(bus, dev, func, 0x00);
    uint16_t vendor = (uint16_t)(id & 0xFFFF);
    if (vendor == 0xFFFF) return;

    uint32_t class_word = pci_read_config(bus, dev, func, 0x08);
    uint32_t header_word = pci_read_config(bus, dev, func, 0x0C);

    pci_device_t *device = &pci_devices[pci_device_count];
    device->bus = bus;
    device->dev = dev;
    device->func = func;
    device->vendor_id = vendor;
    device->device_id = (uint16_t)(id >> 16);
    device->class_code = (uint8_t)(class_word >> 24);
    device->subclass = (uint8_t)(class_word >> 16);
    device->prog_if = (uint8_t)(class_word >> 8);
    device->revision = (uint8_t)(class_word & 0xFF);
    device->header_type = (uint8_t)((header_word >> 16) & 0xFF);

    uint32_t irq_word = pci_read_config(bus, dev, func, 0x3C);
    device->irq = (uint8_t)(irq_word & 0xFF);

    int max_bars = (device->header_type & 0x7F) == 0 ? 6 : 2;
    for (int i = 0; i < max_bars; i++) {
        if (device->header_type & 0x7F) continue;
        uint64_t bar_addr = pci_read_bar(bus, dev, func, i);
        if (bar_addr == 0) {
            device->bar[i] = 0;
            device->bar_size[i] = 0;
            device->bar_is_mmio[i] = 0;
            continue;
        }
        device->bar[i] = bar_addr;
        device->bar_size[i] = pci_get_bar_size(bus, dev, func, i);
        uint32_t bar_low = pci_read_config(bus, dev, func, 0x10 + i * 4);
        device->bar_is_mmio[i] = !(bar_low & 0x01);
        if (device->bar_is_mmio[i] && (bar_low & 0x06) == 0x04) {
            i++;
        }
    }

    pci_device_count++;

    vga_printf("PCI: %02x:%02x.%x VID=%x DID=%x Class=%02x/%02x/%02x",
               bus, dev, func, vendor, device->device_id,
               device->class_code, device->subclass, device->prog_if);

    if (device->class_code == PCI_CLASS_SERIAL_BUS &&
        device->subclass == PCI_SUBCLASS_USB) {
        const char *type = "Unknown";
        if (device->prog_if == PCI_PROG_IF_XHCI) type = "xHCI";
        else if (device->prog_if == PCI_PROG_IF_EHCI) type = "EHCI";
        else if (device->prog_if == PCI_PROG_IF_UHCI) type = "UHCI";
        else if (device->prog_if == PCI_PROG_IF_OHCI) type = "OHCI";
        vga_printf(" [%s]", type);
    }
    vga_printf("\n");

    if ((device->header_type & 0x7F) == 1) {
        uint32_t secondary_bus = (pci_read_config(bus, dev, func, 0x18) >> 8) & 0xFF;
        for (int d = 0; d < PCI_MAX_DEV; d++) {
            uint32_t bid = pci_read_config(secondary_bus, d, 0, 0x00);
            if ((uint16_t)(bid & 0xFFFF) != 0xFFFF) {
                pci_scan_function(secondary_bus, d, 0);
                if (pci_read_config(secondary_bus, d, 0, 0x0C) & 0x800000) {
                    for (int f = 1; f < PCI_MAX_FUNC; f++) {
                        uint32_t fid = pci_read_config(secondary_bus, d, f, 0x00);
                        if ((uint16_t)(fid & 0xFFFF) != 0xFFFF) {
                            pci_scan_function(secondary_bus, d, f);
                        }
                    }
                }
            }
        }
    }
}

void pci_init(void) {
    pci_device_count = 0;
    memset(pci_devices, 0, sizeof(pci_devices));

    for (int bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (int dev = 0; dev < PCI_MAX_DEV; dev++) {
            uint32_t id = pci_read_config(bus, dev, 0, 0x00);
            if ((uint16_t)(id & 0xFFFF) == 0xFFFF) continue;

            pci_scan_function(bus, dev, 0);

            if (pci_read_config(bus, dev, 0, 0x0C) & 0x800000) {
                for (int func = 1; func < PCI_MAX_FUNC; func++) {
                    uint32_t fid = pci_read_config(bus, dev, func, 0x00);
                    if ((uint16_t)(fid & 0xFFFF) != 0xFFFF) {
                        pci_scan_function(bus, dev, func);
                    }
                }
            }
        }
    }

    vga_printf("PCI: Found %d devices\n", pci_device_count);
}

uint64_t pci_get_bar(pci_device_t *dev, int bar_index) {
    if (bar_index < 0 || bar_index >= 6) return 0;
    return dev->bar[bar_index];
}

void pci_enable_device(pci_device_t *dev) {
    uint32_t cmd = pci_read_config(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER;
    pci_write_config(dev->bus, dev->dev, dev->func, 0x04, cmd);
}

int pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                    pci_device_t *out) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass &&
            (prog_if == 0xFF || pci_devices[i].prog_if == prog_if)) {
            if (out) *out = pci_devices[i];
            return 1;
        }
    }
    return 0;
}

int pci_find_all_class(uint8_t class_code, uint8_t subclass,
                       pci_device_t *out, int max_out) {
    int found = 0;
    for (int i = 0; i < pci_device_count && found < max_out; i++) {
        if (pci_devices[i].class_code == class_code &&
            (subclass == 0xFF || pci_devices[i].subclass == subclass)) {
            out[found++] = pci_devices[i];
        }
    }
    return found;
}

int pci_get_device_count(void) {
    return pci_device_count;
}

pci_device_t *pci_get_device(int index) {
    if (index < 0 || index >= pci_device_count) return NULL;
    return &pci_devices[index];
}
