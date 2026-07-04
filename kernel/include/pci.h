#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES 64

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_CLASS_DISPLAY  0x03
#define PCI_CLASS_NETWORK  0x02
#define PCI_CLASS_STORAGE  0x01
#define PCI_CLASS_SERIAL   0x0C
#define PCI_CLASS_BRIDGE   0x06

/* Header type flags */
#define PCI_HEADER_TYPE_MULTI  0x80
#define PCI_HEADER_TYPE_MASK   0x7F

typedef struct {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  revision;
    uint8_t  prog_if;
    uint8_t  subclass;
    uint8_t  class_code;
    uint8_t  header_type;
    uint32_t bars[6];
    uint8_t  interrupt_pin;
    uint8_t  interrupt_line;
} pci_device_t;

void pci_init(void);
uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value);
int pci_get_device_count(void);
const pci_device_t* pci_get_device(int index);
void pci_list_devices(void);

const char* pci_class_name(uint8_t class_code);

#endif /* PCI_H */
