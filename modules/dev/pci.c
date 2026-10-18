/* Immutable inventory and matching policy; platform adapter supplies cached facts. */
#include "sfmod.h"
static const sfm_api_t *core;
static uint32_t owner_id, count;
static uint64_t endpoint;
static sfm_pci_record_t devices[SFM_PCI_MAX];
static int matches(const sfm_pci_query_t *q, const sfm_pci_record_t *d)
{
    return (!(q->match & SFM_PCI_VENDOR) || q->vendor_id == d->vendor_id) &&
        (!(q->match & SFM_PCI_DEVICE) || q->device_id == d->device_id) &&
        (!(q->match & SFM_PCI_CLASS) || q->class_code == d->class_code) &&
        (!(q->match & SFM_PCI_SUBCLASS) || q->subclass == d->subclass) &&
        (!(q->match & SFM_PCI_PROGIF) || q->prog_if == d->prog_if);
}
static int receive(const sfm_message_t *req, sfm_message_t *resp)
{
    if (req->flags != SFM_PCI_VERSION) return -93;
    if (req->type != SFM_PCI_ENUM && req->type != SFM_PCI_FIND && req->type != SFM_PCI_GET) return -38;
    if (req->route || req->offset > count) return -22;
    if (req->type == SFM_PCI_GET) {
        if (req->handle != endpoint || req->offset == count) return -2;
    } else if (req->handle) return -22;
    sfm_pci_query_t query = {0};
    if (req->type == SFM_PCI_FIND) {
        if (req->size != sizeof(query)) return -22;
        sfm_copy(&query, req->data, sizeof(query));
        if (query.match & ~SFM_PCI_MATCH_ALL) return -22;
        for (unsigned i = 0; i < sizeof(query.reserved); i++) if (query.reserved[i]) return -22;
    } else if (req->size) return -22;
    for (uint32_t i = (uint32_t)req->offset; i < count; i++) {
        if (!matches(&query, &devices[i])) continue;
        resp->size = sizeof(devices[i]);
        sfm_copy(resp->data, &devices[i], resp->size);
        resp->handle = endpoint; resp->offset = i; resp->result = 1;
        return 0;
    }
    return 0;
}
static int start(const sfm_api_t *api, uint32_t owner)
{
    if (api->abi != SFM_ABI || api->size < sizeof(*api) || !api->pci_read) return -93;
    core = api; owner_id = owner; count = 0;
    sfm_pci_record_t item;
    for (uint32_t i = 0; i <= SFM_PCI_MAX; i++) {
        int result = core->pci_read(i, &item);
        if (result < 0) return result;
        if (!result) break;
        if (i == SFM_PCI_MAX) return -75;
        if (item.version != SFM_PCI_VERSION || item.size != sizeof(item)) return -93;
        sfm_copy(&devices[count++], &item, sizeof(item));
    }
    endpoint = core->endpoint_create(owner, "dev.pci", receive);
    if (!endpoint) return -12;
    for (uint32_t i = 0; i < count; i++) devices[i].snapshot = endpoint;
    return 0;
}
static void stop(void)
{
    if (endpoint) core->endpoint_destroy(owner_id, endpoint);
    endpoint = 0; count = 0;
}
SFM_MODULE("dev.pci", start, stop);
