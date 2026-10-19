/* Boot-only comparison with the real AHCI/RAMDISK data path. */
#include "block.h"
#include "memory.h"
#include "block_client.h"
#define CHECK(x) do { if (!(x)) goto fail; } while (0)
int sfm_block_io_selftest(void)
{
    sfm_message_t *req = sf_alloc(sizeof(*req)), *resp = sf_alloc(sizeof(*resp));
    uint8_t *reference = alloc_page();
    uint64_t endpoint = sfm_lookup("dev.block");
    int reference_safe = 1;
    CHECK(req && resp && reference && endpoint && sf_block_frozen());
    for (uint32_t index = 0; index < SFM_BLOCK_MAX; index++) {
        sfm_block_record_t record;
        int present = sf_block_read(index, &record);
        CHECK(present >= 0);
        if (!present || record.geometry_source == SFM_BLOCK_GEOMETRY_UNKNOWN ||
            !record.sector_count || record.sector_size != 512) continue;
        sfm_clear(req, sizeof(*req));
        req->type = SFM_BLOCK_READ; req->flags = SFM_BLOCK_VERSION; req->handle = endpoint;
        sfm_block_read_t read = {.index = index, .count = 1};
        req->size = sizeof(read); sfm_copy(req->data, &read, sizeof(read));
        for (unsigned pass = 0; pass < 3; pass++) {
            read.count = pass == 2 && record.sector_count >= 8 ? 8 : 1;
            req->offset = pass == 0 ? 0 : record.sector_count - read.count;
            sfm_copy(req->data, &read, sizeof(read));
            CHECK(!sfm_call(endpoint, req, resp) && resp->result == read.count * 512 &&
                  resp->size == read.count * 512 && resp->handle == endpoint && resp->offset == req->offset);
            reference_safe = 0;
            CHECK(!sf_block_raw_read(index, req->offset, read.count, reference));
            reference_safe = 1;
            for (size_t j = 0; j < resp->size; j++) CHECK(resp->data[j] == reference[j]);
            /* The first test SATA disk contains a FAT boot sector, not zero fill. */
            if (pass == 0 && record.type == SFM_BLOCK_AHCI && sfm_name_equal(record.name, "sda") &&
                resp->data[510] == 0x55 && resp->data[511] == 0xaa)
                sf_log("[sfmod] block READ verified FAT signature\n");
        }
        req->offset = record.sector_count;
        CHECK(sfm_call(endpoint, req, resp) == SFM_EINVAL && !resp->size);
        req->handle ^= 1ull << 32;
        CHECK(sfm_call(endpoint, req, resp) == SFM_ENOENT);
        req->handle = endpoint; req->type = SFM_BLOCK_WRITE;
        CHECK(sfm_call(endpoint, req, resp) == -30);
        /* Never probe writes on attached physical disks. Only exercise the
         * kernel-created volatile ram0, preserving its previous contents. */
        if (record.type == SFM_BLOCK_RAMDISK && record.geometry_source == SFM_BLOCK_GEOMETRY_SOFTWARE &&
            sfm_name_equal(record.name, "ram0") && record.sector_count == 2048) {
            sf_block_client_t disk;
            uint32_t completed;
            uint64_t lba = record.sector_count - 8;
            CHECK(!sf_block_client_open(index, &disk));
            CHECK(!sf_block_client_read(&disk, lba, 8, reference, SFM_PAYLOAD));
            for (unsigned i = 0; i < SFM_PAYLOAD; i++) req->data[i] = (uint8_t)(i % 251 + 1);
            CHECK(!sf_block_client_write_compat(&disk, lba, 8, req->data, SFM_PAYLOAD, &completed) && completed == 8);
            CHECK(!sf_block_client_read(&disk, lba, 8, resp->data, SFM_PAYLOAD));
            for (unsigned i = 0; i < SFM_PAYLOAD; i++) CHECK(resp->data[i] == req->data[i]);
            CHECK(!sf_block_client_write_compat(&disk, lba, 8, reference, SFM_PAYLOAD, &completed) && completed == 8);
            CHECK(!sf_block_client_read(&disk, lba, 8, resp->data, SFM_PAYLOAD));
            for (unsigned i = 0; i < SFM_PAYLOAD; i++) CHECK(resp->data[i] == reference[i]);
            sf_log("[sfmod] compatibility RAM writes use core-owned DMA buffers\n");
        }
    }
    sf_free(req); sf_free(resp); free_page(reference);
    sf_log("[sfmod] read-only block messages match platform sectors\n");
    return 0;
fail:
    sf_free(req); sf_free(resp);
    if (reference && reference_safe) free_page(reference);
    return -1;
}
