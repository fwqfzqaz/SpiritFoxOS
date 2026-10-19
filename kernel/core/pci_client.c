/* AHCI discovery uses only the read-only PCI protocol, never the raw cache. */
#include "pci.h"

int sfm_pci_find_ahci(sfm_pci_record_t *out)
{
    if (!out) return SFM_EINVAL;
    uint64_t endpoint = sfm_lookup("dev.pci");
    if (!endpoint) return SFM_ENOENT;
    sfm_message_t *req = sf_alloc(sizeof(*req)), *resp = sf_alloc(sizeof(*resp));
    int result = SFM_ENOMEM;
    if (!req || !resp) goto done;
    sfm_clear(req, sizeof(*req));
    req->type = SFM_PCI_FIND;
    req->flags = SFM_PCI_VERSION;
    sfm_pci_query_t query = {.match = SFM_PCI_CLASS | SFM_PCI_SUBCLASS | SFM_PCI_PROGIF,
                            .class_code = 1, .subclass = 6, .prog_if = 1};
    req->size = sizeof(query);
    sfm_copy(req->data, &query, sizeof(query));
    for (unsigned tries = 0; tries < SFM_PCI_MAX; tries++) {
        result = sfm_call(endpoint, req, resp);
        if (result) break;
        if (!resp->result && !resp->size) break;
        result = SFM_EINVAL;
        if (resp->result != 1 || resp->size != sizeof(*out) || resp->handle != endpoint ||
            resp->offset < req->offset || resp->offset >= SFM_PCI_MAX) break;
        sfm_pci_record_t record;
        sfm_copy(&record, resp->data, sizeof(record));
        if (record.version != SFM_PCI_VERSION || record.size != sizeof(record) ||
            record.snapshot != endpoint || record.vendor_id == 0xffff ||
            record.device > 31 || record.function > 7 || (record.header_type & 0x7f) ||
            record.class_code != 1 || record.subclass != 6 || record.prog_if != 1) break;
        /* BAR5 cannot begin a 64-bit pair. The 0x1100 span is driver knowledge,
         * not a BAR-size probe. Reject I/O, upper halves and malformed flags. */
        const sfm_pci_bar_t *bar = &record.bars[5];
        uint32_t allowed = SFM_BAR_PRESENT | SFM_BAR_MEM32 | SFM_BAR_PREFETCH | SFM_BAR_SIZE_KNOWN;
        if ((bar->flags & (SFM_BAR_PRESENT | SFM_BAR_MEM32)) == (SFM_BAR_PRESENT | SFM_BAR_MEM32) &&
            !(bar->flags & ~allowed) && bar->address && !(bar->address & 15) &&
            bar->address <= UINT32_MAX - 0x10ffu &&
            (!(bar->flags & SFM_BAR_SIZE_KNOWN) || bar->size >= 0x1100)) {
            *out = record;
            result = 1;
            break;
        }
        req->offset = resp->offset + 1;
        result = 0;
    }
done:
    sf_free(req); sf_free(resp);
    return result;
}
