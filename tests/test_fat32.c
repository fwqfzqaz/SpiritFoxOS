/* Production FAT32 -> production client -> real RPC -> synthetic disk. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/core/internal.h"
#include "../kernel/src/fs/fat32.c"
#include "../kernel/core/block_io.c" /* Production DMA guard; fixture reset models reboot only. */

static uint8_t image[512 * 512];
static uint64_t endpoint;
static unsigned reads, fail_read, fail_mode;
static unsigned writes, fail_write;
static int write_error;
static vfs_file_t *nested_file;
static vfs_dentry_t *nested_parent;
static _Alignas(4096) uint8_t dma[2][4096];
static unsigned dma_count;
static int fail_pages, page_budget = -1;
static size_t live_pages, live_allocs;
static struct { uint8_t *base; unsigned pages, freed; uint32_t mask; } extents[512];
void *sf_alloc(size_t size) { void *p = malloc(size); if (p) live_allocs++; return p; }
void sf_free(void *p) { if (p) { assert(live_allocs); live_allocs--; free(p); } }
void sf_log(const char *s) { (void)s; }
uint64_t sf_irq_save(void) { return 0; }
void sf_irq_restore(uint64_t irq) { (void)irq; }
void *alloc_pages(size_t count)
{
    if (fail_pages || !page_budget) return NULL;
    if (page_budget > 0) page_budget--;
    assert(count && count <= 16);
    for (unsigned i = 0; i < 512; i++) if (!extents[i].base) {
        extents[i].base = aligned_alloc(PAGE_SIZE, count * PAGE_SIZE); assert(extents[i].base);
        extents[i].pages = count; extents[i].freed = 0; extents[i].mask = 0;
        memset(extents[i].base, 0xa5, count * PAGE_SIZE); live_pages += count;
        return extents[i].base;
    }
    abort();
}
void *alloc_page(void) { return alloc_pages(1); }
void free_page(void *p)
{
    uintptr_t value = (uintptr_t)p;
    for (unsigned i = 0; i < 512; i++) {
        uintptr_t base = (uintptr_t)extents[i].base;
        if (!base || value < base || value - base >= extents[i].pages * PAGE_SIZE) continue;
        assert(!((value - base) % PAGE_SIZE));
        unsigned n = (value - base) / PAGE_SIZE;
        assert(!(extents[i].mask & (1u << n)));
        extents[i].mask |= 1u << n; extents[i].freed++; live_pages--;
        if (extents[i].freed == extents[i].pages) { free(extents[i].base); extents[i].base = NULL; }
        return;
    }
    abort();
}
int vfs_register_fs(vfs_filesystem_t *fs) { assert(fs == &fat32_fs); return 0; }
int sf_block_frozen(void) { return 1; }
void *sf_block_dma_page(void) { assert(dma_count < 2); return dma[dma_count++]; }
int sf_block_read(uint32_t index, sfm_block_record_t *out)
{
    if (index) return 0;
    *out = (sfm_block_record_t){.sector_count = 512, .sector_size = 512,
        .type = SFM_BLOCK_AHCI, .geometry_source = SFM_BLOCK_GEOMETRY_ATA};
    return 1;
}
int sf_block_raw_read(uint32_t id, uint64_t lba, uint32_t count, void *buf)
{
    assert(!id && lba <= 512 && count <= 512 - lba && count <= 8);
    memcpy(buf, image + lba * 512, count * 512); return 0;
}
int sf_block_raw_write(uint32_t id, uint64_t lba, uint32_t count, const void *buf)
{
    assert(!id && lba <= 512 && count <= 512 - lba && count <= 8);
    assert(buf == write_pages[0] && !((uintptr_t)buf & 4095));
    if (nested_file) {
        uint8_t byte = 0;
        fat32_inode_data_t private = {0}; vfs_inode_t inode = {.fs_data = &private};
        assert(fat32_inode_write(nested_file, &byte, 1) == SFM_EBUSY);
        assert(fat32_create(nested_parent, "nested", &inode) == SFM_EBUSY);
        assert(fat32_mkdir_cb(nested_parent, "nested", &inode) == SFM_EBUSY);
        assert(fat32_unlink_cb(nested_parent->child) == SFM_EBUSY);
        assert(((fat32_sb_data_t *)nested_file->inode->sb->fs_data)->write_busy);
    }
    writes++;
    if (writes == fail_write) {
        if (write_error != SFM_EBUSY) memcpy(image + lba * 512, buf, count * 256);
        return write_error;
    }
    memcpy(image + lba * 512, buf, count * 512); return 0;
}
static int reply(const sfm_message_t *req, sfm_message_t *resp)
{
    assert(req->flags == SFM_BLOCK_VERSION && req->handle == endpoint);
    resp->handle = endpoint;
    if (req->type == SFM_BLOCK_GET) {
        sfm_block_record_t r = {.version = SFM_BLOCK_VERSION, .size = sizeof(r),
            .snapshot = endpoint, .sector_count = 512, .sector_size = 512,
            .type = SFM_BLOCK_AHCI, .geometry_source = SFM_BLOCK_GEOMETRY_ATA};
        resp->size = sizeof(r); resp->result = 1; memcpy(resp->data, &r, sizeof(r)); return 0;
    }
    assert(req->type == SFM_BLOCK_READ);
    sfm_block_read_t r; memcpy(&r, req->data, sizeof(r));
    assert(!r.index && r.count && r.count <= 8 && req->offset + r.count <= 512);
    reads++;
    resp->offset = req->offset; resp->size = r.count * 512; resp->result = resp->size;
    int io_result = sf_block_read_sectors(0, req->offset, r.count, resp->data, SFM_PAYLOAD);
    if (io_result) return io_result;
    if (reads == fail_read) {
        if (fail_mode == 1) return SFM_EBUSY;
        if (fail_mode == 2) { resp->size--; return 0; }
        if (fail_mode == 3) { resp->handle++; return 0; }
        if (fail_mode == 4) return SFM_EAGAIN;
        return -5;
    }
    return 0;
}
static void fixture(void)
{
    memset(image, 0, sizeof(image)); reads = 0; fail_read = 0; writes = 0; fail_write = 0;
    memset(failed, 0, sizeof(failed)); /* Synthetic fresh controller, never a runtime recovery API. */
    fat32_bpb_t *b = (void *)image;
    b->signature = 0xaa55; b->bytes_per_sector = 512; b->sectors_per_cluster = 16;
    b->reserved_sectors = 1; b->num_fats = 1; b->sectors_per_fat_32 = 1;
    b->root_dir_cluster = 2; b->total_sectors_32 = 512; memcpy(b->fs_type, "FAT32   ", 8);
    uint32_t *fat = (void *)(image + 512); fat[2] = 0x0fffffff; fat[3] = 4; fat[4] = 0x0fffffff;
    fat32_dir_entry_t *dir = (void *)(image + 2 * 512);
    memcpy(dir->name, "TEST    BIN", 11); dir->cluster_lo = 3; dir->file_size = 9000;
    for (unsigned i = 0; i < 9000; i++) image[18 * 512 + i] = (uint8_t)(i % 251 + 1);
}
static void dispose(vfs_superblock_t *s)
{
    fat32_discard_tree(s->root); free_page(s->fs_data); assert(!live_pages);
}
static void test_writes(void)
{
    uint8_t payload[10000]; memset(payload, 0x6a, sizeof(payload));
    for (unsigned busy_error = 0; busy_error < 2; busy_error++) {
        for (unsigned segment = 1; segment <= 6; segment++) {
            fixture(); vfs_superblock_t s = {0}; assert(!fat32_mount(&s, 0, NULL));
            vfs_file_t f = {.inode = s.root->child->inode, .offset = 8000};
            fat32_inode_data_t *id = f.inode->fs_data;
            fail_write = segment; write_error = busy_error ? SFM_EBUSY : -5;
            /* First cluster tail, second cluster, then directory growth metadata. */
            int result = fat32_inode_write(&f, payload, 2000);
            assert(result < 0 && f.offset == 8000 && id->file_size == 9000 && f.inode->size == 9000);
            unsigned before = writes;
            if (busy_error && segment == 1) {
                assert(!((fat32_sb_data_t *)s.fs_data)->write_fault);
                fail_write = 0;
                assert(fat32_inode_write(&f, payload, 2000) == 2000 && f.offset == 10000);
            } else {
                assert(((fat32_sb_data_t *)s.fs_data)->write_fault);
                assert(fat32_inode_write(&f, payload, 1) == -30 && writes == before);
            }
            dispose(&s);
        }
    }
    fixture(); vfs_superblock_t s = {0}; assert(!fat32_mount(&s, 0, NULL));
    vfs_file_t f = {.inode = s.root->child->inode};
    assert(!fat32_inode_write(&f, NULL, 0) && !writes);
    f.offset = 9001;
    assert(fat32_inode_write(&f, payload, 1) == -22 && !writes); /* Reject sparse holes. */
    uint32_t out = 0xa5a5a5a5;
    page_budget = 0;
    assert(fat32_append_cluster(s.fs_data, 4, &out) == -12 && !writes && out == 0xa5a5a5a5);
    page_budget = -1;
    fail_write = 2; write_error = -5; /* Reservation succeeded, zeroing failed. */
    assert(fat32_append_cluster(s.fs_data, 4, &out) < 0 && out == 0xa5a5a5a5);
    assert((((uint32_t *)(image + 512))[4] & 0x0fffffff) == 0x0fffffff); /* Never linked. */
    dispose(&s);

    fixture(); assert(!fat32_mount(&s, 0, NULL));
    fail_write = 1; write_error = SFM_EBUSY;
    assert(fat32_unlink_cb(s.root->child) == SFM_EBUSY && writes == 1);
    assert(((uint32_t *)(image + 512))[3] == 4); /* Failed deletion cannot free a live file. */
    fail_write = 0;
    assert(!fat32_unlink_cb(s.root->child));
    assert(!((uint32_t *)(image + 512))[3] && !((uint32_t *)(image + 512))[4]);
    dispose(&s);

    for (unsigned fail_copy = 0; fail_copy < 2; fail_copy++) {
        fixture();
        memmove(image + 3 * 512, image + 2 * 512, sizeof(image) - 3 * 512);
        memcpy(image + 2 * 512, image + 512, 512);
        ((fat32_bpb_t *)image)->num_fats = 2;
        ((uint32_t *)(image + 512))[5] = 0xa0000000;
        ((uint32_t *)(image + 1024))[5] = 0xb0000000;
        assert(!fat32_mount(&s, 0, NULL));
        if (fail_copy) { fail_write = 2; write_error = SFM_EBUSY; }
        int result = fat32_set_fat_entry(s.fs_data, 5, 0x0fffffff);
        assert(((uint32_t *)(image + 512))[5] == 0xafffffff);
        if (fail_copy) {
            assert(result == SFM_EBUSY && ((fat32_sb_data_t *)s.fs_data)->write_fault);
            assert(((uint32_t *)(image + 1024))[5] == 0xb0000000);
        } else assert(!result && ((uint32_t *)(image + 1024))[5] == 0xbfffffff);
        dispose(&s);
    }
    fixture(); assert(!fat32_mount(&s, 0, NULL));
    f = (vfs_file_t){.inode = s.root->child->inode};
    assert(fat32_inode_write(&f, payload, sizeof(payload)) == sizeof(payload));
    uint8_t actual[10000]; f.offset = 0;
    assert(fat32_inode_read(&f, actual, sizeof(actual)) == sizeof(actual) && !memcmp(actual, payload, sizeof(actual)));
    dispose(&s);
    fixture();
    fat32_dir_entry_t *entry = (void *)(image + 2 * 512);
    entry->cluster_lo = 0; entry->file_size = 0;
    memset(image + 50 * 512, 0xcc, 2 * 8192); /* Old data in free clusters 5 and 6. */
    assert(!fat32_mount(&s, 0, NULL));
    f = (vfs_file_t){.inode = s.root->child->inode};
    nested_file = &f; nested_parent = s.root;
    assert(fat32_inode_write(&f, payload, sizeof(payload)) == sizeof(payload));
    nested_file = NULL; nested_parent = NULL;
    assert(!((fat32_sb_data_t *)s.fs_data)->write_busy);
    assert(((fat32_inode_data_t *)f.inode->fs_data)->start_cluster == 5);
    assert(((uint32_t *)(image + 512))[5] == 6);
    for (unsigned i = 10000 - 8192; i < 8192; i++) assert(!image[66 * 512 + i]);
    f.offset = 0;
    assert(fat32_inode_read(&f, actual, sizeof(actual)) == sizeof(actual) && !memcmp(actual, payload, sizeof(actual)));
    dispose(&s);
    puts("FAT32 compatibility writes passed: owned DMA, busy/partial/error, metadata publication, mirrors, zero-before-link, unlink");
}
int main(void)
{
    sfm_owner_enable(SFM_CORE_OWNER, 1);
    endpoint = sfm_endpoint_create(SFM_CORE_OWNER, "dev.block", reply); assert(endpoint);
    for (unsigned bad = 0; bad < 9; bad++) {
        fixture(); fat32_bpb_t *b = (void *)image;
        switch (bad) {
        case 0: b->sectors_per_cluster = 0; break;
        case 1: b->sectors_per_cluster = 3; break;
        case 2: b->bytes_per_sector = 4096; break;
        case 3: b->total_sectors_32 = 1; break;
        case 4: b->total_sectors_32 = 513; break;
        case 5: b->root_dir_cluster = 999; break;
        case 6: b->num_fats = 0; break;
        case 7: b->sectors_per_cluster = 1; break; /* FAT table too small. */
        case 8: b->ext_flags = 0x80; break;
        }
        vfs_superblock_t sb = {0}; assert(fat32_mount(&sb, 0, NULL) < 0 && !live_pages);
    }
    for (fail_mode = 0; fail_mode <= 4; fail_mode++) {
        for (unsigned fail = 1; fail <= 3; fail++) {
            fixture(); fail_read = fail;
            vfs_superblock_t sb = {0}; assert(fat32_mount(&sb, 0, NULL) < 0 && !live_pages);
        }
    }
    for (int budget = 0; budget < 9; budget++) {
        fixture(); page_budget = budget;
        vfs_superblock_t failed = {0};
        assert(fat32_mount(&failed, 0, NULL) < 0 && !live_pages && !failed.fs_data && !failed.root);
        page_budget = -1;
    }
    fixture(); memset(image + 2 * 512, 0xe5, 8192);
    ((uint32_t *)(image + 512))[2] = 2; /* Cyclic directory FAT chain. */
    vfs_superblock_t cyclic = {0};
    assert(fat32_mount(&cyclic, 0, NULL) < 0 && !live_pages && !cyclic.root);
    fixture(); fat32_dir_entry_t *loop = (void *)(image + 2 * 512);
    loop->attr = FAT32_ATTR_DIRECTORY; loop->cluster_lo = 2; /* Recursive directory cycle. */
    assert(fat32_mount(&cyclic, 0, NULL) < 0 && !live_pages && !cyclic.root);
    fixture(); vfs_superblock_t sb = {0}; assert(!fat32_mount(&sb, 0, NULL));
    assert(reads == 3 && sb.root->child && !strcmp(sb.root->child->name, "test.bin"));
    vfs_file_t file = {.inode = sb.root->child->inode};
    uint8_t buffer[9000]; assert(fat32_inode_read(&file, buffer, sizeof(buffer)) == 9000);
    for (unsigned i = 0; i < sizeof(buffer); i++) assert(buffer[i] == (uint8_t)(i % 251 + 1));
    assert(file.offset == 9000);
    for (fail_mode = 0; fail_mode <= 4; fail_mode++) {
        for (unsigned fail = 1; fail <= 5; fail++) {
            reads = 0; fail_read = fail; file.offset = 0; memset(buffer, 0xa5, sizeof(buffer));
            assert(fat32_inode_read(&file, buffer, sizeof(buffer)) < 0 && !file.offset);
            for (unsigned i = 0; i < sizeof(buffer); i++) assert(!buffer[i]);
        }
    }
    /* FAT lookup errors cannot look like EOC or allocate/write a new chain. */
    reads = 0; fail_read = 1; uint32_t out = 0xa5a5a5a5;
    assert(fat32_get_fat_entry(sb.fs_data, 3, &out) < 0 && out == 0xa5a5a5a5);
    fail_read = 0; fail_pages = 1;
    assert(fat32_get_fat_entry(sb.fs_data, 3, &out) == -12 && out == 0xa5a5a5a5);
    fail_pages = 0;
    fat32_discard_tree(sb.root); free_page(sb.fs_data); assert(!live_pages);
    fixture();
    ((fat32_bpb_t *)image)->sectors_per_cluster = 128;
    memmove(image + 130 * 512, image + 18 * 512, sizeof(buffer));
    vfs_superblock_t large = {0};
    assert(!fat32_mount(&large, 0, NULL) && reads == 17); /* BPB + 16 messages for a 64KiB root. */
    file = (vfs_file_t){.inode = large.root->child->inode};
    assert(fat32_inode_read(&file, buffer, sizeof(buffer)) == 9000);
    for (unsigned i = 0; i < sizeof(buffer); i++) assert(buffer[i] == (uint8_t)(i % 251 + 1));
    fat32_discard_tree(large.root); free_page(large.fs_data); assert(!live_pages);
    test_writes();
    assert(!sfm_endpoint_destroy(SFM_CORE_OWNER, endpoint) && !live_allocs);
    puts("FAT32 message reads passed: 8/64KiB clusters, malformed BPB, partial/error/stale/busy/OOM, cycles, cleanup");
    return 0;
}
