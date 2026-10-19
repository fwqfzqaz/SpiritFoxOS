/* Exercise the exact selector used by ahci_init, through real endpoint RPC. */
#include <assert.h>
#include <string.h>
#include "../kernel/core/pci.h"

extern void test_fail_allocations(int);
static uint64_t endpoint;
static unsigned mode, calls;

static int reply(const sfm_message_t *req, sfm_message_t *resp)
{
    sfm_pci_query_t query;
    assert(req->type == SFM_PCI_FIND && req->flags == SFM_PCI_VERSION &&
           req->size == sizeof(query) && !req->route);
    memcpy(&query, req->data, sizeof(query));
    assert(query.match == (SFM_PCI_CLASS | SFM_PCI_SUBCLASS | SFM_PCI_PROGIF) &&
           query.class_code == 1 && query.subclass == 6 && query.prog_if == 1);
    calls++;
    if (mode == 1) return 0; /* No AHCI. */
    if (mode == 2) return SFM_EBUSY;
    sfm_pci_record_t record = {.version = SFM_PCI_VERSION, .size = sizeof(record),
        .snapshot = endpoint, .vendor_id = 0x8086, .device = 3,
        .class_code = 1, .subclass = 6, .prog_if = 1};
    record.bars[5].flags = SFM_BAR_PRESENT | SFM_BAR_MEM32;
    record.bars[5].address = 0xfebf0000;
    resp->result = 1; resp->handle = endpoint;
    resp->offset = req->offset; resp->size = sizeof(record);
    switch (mode) {
    case 3: record.version++; break;
    case 4: record.size--; break;
    case 5: record.snapshot++; break;
    case 6: resp->handle++; break;
    case 7: resp->size--; break;
    case 8: resp->offset = SFM_PCI_MAX; break;
    case 9: record.class_code = 3; break;
    case 10: record.device = 32; break;
    case 11: record.function = 8; break;
    case 12: record.header_type = 1; break;
    case 13: record.vendor_id = 0xffff; break;
    case 14: resp->result = 2; break;
    case 15: resp->result = 0; break;
    /* Skip an unusable candidate and select the next one. */
    case 16: if (!req->offset) record.bars[5].flags = SFM_BAR_PRESENT | SFM_BAR_IO; break;
    case 17: record.bars[5].flags |= SFM_BAR_MEM64; break;
    case 18: record.bars[5].flags |= SFM_BAR_UPPER; break;
    case 19: record.bars[5].flags |= SFM_BAR_INVALID; break;
    case 20: record.bars[5].flags |= SFM_BAR_UNKNOWN; break;
    case 21: record.bars[5].address = 0; break;
    case 22: record.bars[5].address = 0x100000000ull; break;
    case 23: record.bars[5].address = 0xfffffff0; break;
    case 24: record.bars[5].address++; break;
    case 25: record.bars[5].flags |= SFM_BAR_SIZE_KNOWN; record.bars[5].size = 0x1000; break;
    case 26: record.bars[5].flags |= SFM_BAR_SIZE_KNOWN; record.bars[5].size = 0x1100; break;
    case 27: /* A non-progressing server after a skipped candidate. */
        record.bars[5].flags = SFM_BAR_IO; resp->offset = 0; break;
    }
    memcpy(resp->data, &record, sizeof(record));
    return 0;
}

void test_pci_client(void)
{
    sfm_pci_record_t record, saved;
    memset(&saved, 0xa5, sizeof(saved)); record = saved;
    assert(sfm_pci_find_ahci(NULL) == SFM_EINVAL);
    assert(sfm_pci_find_ahci(&record) == SFM_ENOENT);
    sfm_owner_enable(SFM_CORE_OWNER, 1);
    endpoint = sfm_endpoint_create(SFM_CORE_OWNER, "dev.pci", reply);
    assert(endpoint);
    for (int budget = 0; budget < 2; budget++) {
        test_fail_allocations(budget);
        assert(sfm_pci_find_ahci(&record) == SFM_ENOMEM);
        test_fail_allocations(-1);
        assert(!memcmp(&record, &saved, sizeof(record)));
    }
    for (mode = 0; mode <= 27; mode++) {
        record = saved; calls = 0;
        int result = sfm_pci_find_ahci(&record);
        if (!mode || mode == 16 || mode == 26) {
            assert(result == 1 && record.bars[5].address == 0xfebf0000);
            assert(calls == (mode == 16 ? 2u : 1u));
        } else {
            assert(!memcmp(&record, &saved, sizeof(record)));
            if (mode == 2) assert(result == SFM_EBUSY);
            else if (mode >= 3 && mode <= 15) assert(result == SFM_EINVAL);
            else if (mode == 27) assert(result == SFM_EINVAL && calls == 2);
            else assert(!result && calls == (mode == 1 ? 1u : SFM_PCI_MAX));
        }
    }
    assert(!sfm_endpoint_destroy(SFM_CORE_OWNER, endpoint));
    sfm_owner_enable(SFM_CORE_OWNER, 0);
}
