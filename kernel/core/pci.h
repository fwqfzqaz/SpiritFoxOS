#ifndef SFM_PCI_INTERNAL_H
#define SFM_PCI_INTERNAL_H
#include "internal.h"
#include "../include/pci.h"
int sfm_pci_describe(const pci_device_t *, sfm_pci_record_t *);
int sfm_pci_selftest(void);
/* 1: selected controller, 0: none usable, negative: service/protocol error. */
int sfm_pci_find_ahci(sfm_pci_record_t *);
#endif
