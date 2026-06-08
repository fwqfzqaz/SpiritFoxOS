#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_MAX_BUS   256
#define PCI_MAX_DEV   32
#define PCI_MAX_FUNC  8

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB     0x03
#define PCI_PROG_IF_XHCI     0x30
#define PCI_PROG_IF_EHCI     0x20
#define PCI_PROG_IF_UHCI     0x00
#define PCI_PROG_IF_OHCI     0x10

#define PCI_CMD_IO_SPACE     (1 << 0)
#define PCI_CMD_MEMORY_SPACE (1 << 1)
#define PCI_CMD_BUS_MASTER   (1 << 2)

typedef struct {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint8_t  irq;
    uint64_t bar[6];
    uint32_t bar_size[6];
    int      bar_is_mmio[6];
} pci_device_t;

#define PCI_MAX_DEVICES 128

void pci_init(void);
uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value);
uint64_t pci_get_bar(pci_device_t *dev, int bar_index);
void pci_enable_device(pci_device_t *dev);
int pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                    pci_device_t *out);
int pci_find_all_class(uint8_t class_code, uint8_t subclass,
                       pci_device_t *out, int max_out);
int pci_get_device_count(void);
pci_device_t *pci_get_device(int index);

#endif
