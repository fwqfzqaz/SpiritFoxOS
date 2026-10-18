/* Decode cached BARs without probing or modifying active hardware. */
#include "pci.h"
int sfm_pci_describe(const pci_device_t *device, sfm_pci_record_t *out)
{
    if (!device || !out || device->device > 31 || device->function > 7 ||
        device->vendor_id == 0xffff) return SFM_EINVAL;
    sfm_clear(out, sizeof(*out));
    out->version = SFM_PCI_VERSION; out->size = sizeof(*out);
    out->vendor_id = device->vendor_id; out->device_id = device->device_id;
    out->bus = device->bus; out->device = device->device; out->function = device->function;
    out->class_code = device->class_code; out->subclass = device->subclass;
    out->prog_if = device->prog_if; out->revision = device->revision;
    out->header_type = device->header_type;
    out->interrupt_line = device->interrupt_line; out->interrupt_pin = device->interrupt_pin;
    for (unsigned i = 0; i < 6; i++) {
        sfm_pci_bar_t *bar = &out->bars[i];
        if ((device->header_type & PCI_HEADER_TYPE_MASK) != 0) {
            bar->flags = SFM_BAR_UNKNOWN;
            continue; /* Legacy scanner caches only type-0 BARs. */
        }
        uint32_t raw = device->bars[i];
        if (!raw) continue;
        if (raw == UINT32_MAX) { bar->flags = SFM_BAR_INVALID; continue; }
        bar->flags = SFM_BAR_PRESENT;
        if (raw & 1u) {
            bar->flags |= SFM_BAR_IO; bar->address = raw & ~3u;
        } else {
            uint32_t type = (raw >> 1) & 3u;
            if (type != 0 && type != 2) { bar->flags |= SFM_BAR_UNKNOWN; continue; }
            bar->flags |= (raw & 8u) ? SFM_BAR_PREFETCH : 0;
            bar->address = raw & ~15u;
            if (type == 2) {
                if (i == 5) { bar->flags = SFM_BAR_INVALID; bar->address = 0; continue; }
                bar->address |= (uint64_t)device->bars[i + 1] << 32;
                bar->flags |= SFM_BAR_MEM64;
                out->bars[++i].flags = SFM_BAR_UPPER;
            } else bar->flags |= SFM_BAR_MEM32;
        }
        /* size=0, SIZE_KNOWN clear: no destructive write-all-ones probing. */
    }
    return 0;
}
