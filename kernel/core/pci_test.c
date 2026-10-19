/* Same protocol checks execute against real modules on host and during boot. */
#include "pci.h"
#define CHECK(x) do { if (!(x)) goto fail; } while (0)
int sfm_pci_selftest(void)
{
    sfm_message_t *req = sf_alloc(sizeof(*req)), *resp = sf_alloc(sizeof(*resp));
    uint64_t endpoint = sfm_lookup("dev.pci");
    CHECK(req && resp && endpoint);
    uint32_t count = 0;
    for (; count <= SFM_PCI_MAX; count++) {
        sfm_pci_record_t expected;
        int available = sf_pci_read(count, &expected);
        CHECK(available >= 0);
        sfm_clear(req, sizeof(*req));
        req->type = SFM_PCI_ENUM; req->flags = SFM_PCI_VERSION; req->offset = count;
        CHECK(!sfm_call(endpoint, req, resp));
        if (!available) { CHECK(!resp->result && !resp->size); break; }
        CHECK(resp->result == 1 && resp->size == sizeof(expected) &&
              resp->handle == endpoint && resp->offset == count);
        expected.snapshot = endpoint;
        for (size_t j = 0; j < sizeof(expected); j++)
            CHECK(resp->data[j] == ((uint8_t *)&expected)[j]);
        req->type = SFM_PCI_GET; req->handle = endpoint;
        CHECK(!sfm_call(endpoint, req, resp) && resp->result == 1);
        req->handle ^= 1ull << 32;
        CHECK(sfm_call(endpoint, req, resp) == SFM_ENOENT);
    }
    CHECK(count <= SFM_PCI_MAX);
    sfm_clear(req, sizeof(*req));
    req->type = SFM_PCI_ENUM; req->flags = SFM_PCI_VERSION; req->offset = count + 1;
    CHECK(sfm_call(endpoint, req, resp) == SFM_EINVAL);
    req->offset = UINT64_MAX;
    CHECK(sfm_call(endpoint, req, resp) == SFM_EINVAL);
    req->offset = 0; req->size = 1;
    CHECK(sfm_call(endpoint, req, resp) == SFM_EINVAL);
    req->size = 0; req->flags++;
    CHECK(sfm_call(endpoint, req, resp) == -93);
    req->flags = SFM_PCI_VERSION; req->type = 0xffff;
    CHECK(sfm_call(endpoint, req, resp) == SFM_ENOSYS);
    req->type = SFM_PCI_FIND;
    req->size = sizeof(sfm_pci_query_t) - 1;
    CHECK(sfm_call(endpoint, req, resp) == SFM_EINVAL);
    req->size++;
    sfm_pci_query_t query = {.match = 0x80000000};
    sfm_copy(req->data, &query, sizeof(query));
    CHECK(sfm_call(endpoint, req, resp) == SFM_EINVAL);
    query.match = 0; query.reserved[0] = 1;
    sfm_copy(req->data, &query, sizeof(query));
    CHECK(sfm_call(endpoint, req, resp) == SFM_EINVAL);
    sfm_clear(&query, sizeof(query));
    query.match = SFM_PCI_VENDOR; query.vendor_id = 0xffff;
    sfm_copy(req->data, &query, sizeof(query));
    CHECK(!sfm_call(endpoint, req, resp) && !resp->result && !resp->size);
    if (count) {
        sfm_pci_record_t first;
        CHECK(sf_pci_read(0, &first) == 1);
        query.match = SFM_PCI_MATCH_ALL;
        query.vendor_id = first.vendor_id; query.device_id = first.device_id;
        query.class_code = first.class_code; query.subclass = first.subclass; query.prog_if = first.prog_if;
        sfm_copy(req->data, &query, sizeof(query));
        CHECK(!sfm_call(endpoint, req, resp) && resp->result == 1 && resp->offset == 0);
    }
    /* An unfiltered FIND must return all matches in index order, without repeats. */
    sfm_clear(&query, sizeof(query));
    sfm_copy(req->data, &query, sizeof(query));
    for (uint32_t i = 0; i <= count; i++) {
        req->offset = i;
        CHECK(!sfm_call(endpoint, req, resp));
        CHECK(i == count ? !resp->result : resp->result == 1 && resp->offset == i);
    }
    sf_free(req); sf_free(resp);
    sf_log("[sfmod] PCI snapshot matches platform inventory\n");
    return 0;
fail:
    sf_free(req); sf_free(resp);
    sf_log("[sfmod] PCI snapshot self-test failed\n");
    return -1;
}
