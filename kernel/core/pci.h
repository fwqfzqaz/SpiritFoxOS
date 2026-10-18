#ifndef SFM_PCI_INTERNAL_H
#define SFM_PCI_INTERNAL_H
#include "internal.h"
#include "../include/pci.h"
int sfm_pci_describe(const pci_device_t *, sfm_pci_record_t *);
int sfm_pci_selftest(void);
#endif
