/* Production FAT32 -> production client -> real RPC -> synthetic disk. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/core/internal.h"
#include "../kernel/src/fs/fat32.c"

static uint8_t image[512 * 512];
static uint64_t endpoint;
static unsigned reads, fail_read, fail_mode;
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
int blkdev_write(uint8_t id, uint64_t lba, uint32_t count, const void *buf)
{
    assert(!id && lba <= 512 && count <= 512 - lba);
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
    memcpy(resp->data, image + req->offset * 512, resp->size);
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
    memset(image, 0, sizeof(image)); reads = 0; fail_read = 0;
    fat32_bpb_t *b = (void *)image;
    b->signature = 0xaa55; b->bytes_per_sector = 512; b->sectors_per_cluster = 16;
    b->reserved_sectors = 1; b->num_fats = 1; b->sectors_per_fat_32 = 1;
    b->root_dir_cluster = 2; b->total_sectors_32 = 512; memcpy(b->fs_type, "FAT32   ", 8);
    uint32_t *fat = (void *)(image + 512); fat[2] = 0x0fffffff; fat[3] = 4; fat[4] = 0x0fffffff;
    fat32_dir_entry_t *dir = (void *)(image + 2 * 512);
    memcpy(dir->name, "TEST    BIN", 11); dir->cluster_lo = 3; dir->file_size = 9000;
    for (unsigned i = 0; i < 9000; i++) image[18 * 512 + i] = (uint8_t)(i % 251 + 1);
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
    assert(!sfm_endpoint_destroy(SFM_CORE_OWNER, endpoint) && !live_allocs);
    puts("FAT32 message reads passed: 8/64KiB clusters, malformed BPB, partial/error/stale/busy/OOM, cycles, cleanup");
    return 0;
}
