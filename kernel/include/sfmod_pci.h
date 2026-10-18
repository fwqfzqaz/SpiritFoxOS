/* Read-only domain-0 inventory; no config writes or mapping rights. */
#ifndef SFM_PCI_H
#define SFM_PCI_H
#include <stdint.h>
#include <stddef.h>
#define SFM_PCI_VERSION 1u
#define SFM_PCI_MAX 64u
#define SFM_PCI_ENUM 0x3000u
#define SFM_PCI_FIND 0x3001u
#define SFM_PCI_GET 0x3002u
#define SFM_PCI_VENDOR 1u
#define SFM_PCI_DEVICE 2u
#define SFM_PCI_CLASS 4u
#define SFM_PCI_SUBCLASS 8u
#define SFM_PCI_PROGIF 16u
#define SFM_PCI_MATCH_ALL 31u
#define SFM_BAR_PRESENT 1u
#define SFM_BAR_IO 2u
#define SFM_BAR_MEM32 4u
#define SFM_BAR_MEM64 8u
#define SFM_BAR_PREFETCH 16u
#define SFM_BAR_INVALID 32u
#define SFM_BAR_UPPER 64u
#define SFM_BAR_SIZE_KNOWN 128u
#define SFM_BAR_UNKNOWN 256u
typedef struct sfm_pci_bar {
    uint64_t address, size;
    uint32_t flags, reserved;
} sfm_pci_bar_t;
typedef struct sfm_pci_record {
    uint32_t version, size;
    uint64_t snapshot;
    uint16_t vendor_id, device_id;
    uint8_t bus, device, function, class_code, subclass, prog_if, revision, header_type;
    uint8_t interrupt_line, interrupt_pin;
    uint16_t reserved;
    sfm_pci_bar_t bars[6];
} sfm_pci_record_t;
typedef struct sfm_pci_query {
    uint32_t match;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if, reserved[5];
} sfm_pci_query_t;
_Static_assert(sizeof(sfm_pci_bar_t) == 24, "PCI BAR ABI");
_Static_assert(offsetof(sfm_pci_record_t, bars) == 32, "PCI header ABI");
_Static_assert(sizeof(sfm_pci_record_t) == 176, "PCI record ABI");
_Static_assert(sizeof(sfm_pci_query_t) == 16, "PCI query ABI");
#endif
