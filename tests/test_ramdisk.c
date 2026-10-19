/* Compile the production ramdisk and block dispatcher against a poisoned PMM. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/include/memory.h"
#include "../kernel/include/ramdisk.h"
#include "../kernel/include/blkdev.h"

static struct extent { uint8_t *base; size_t pages, freed; unsigned char released[1024]; } extents[8];
static size_t live_pages, allocation_calls;
static int fail_allocation;
void *alloc_pages(size_t count)
{
    allocation_calls++;
    assert(count && count <= 1024);
    if (fail_allocation) return NULL;
    for (unsigned i = 0; i < 8; i++) if (!extents[i].base) {
        struct extent *e = &extents[i];
        memset(e, 0, sizeof(*e));
        e->base = aligned_alloc(PAGE_SIZE, count * PAGE_SIZE);
        assert(e->base);
        e->pages = count; live_pages += count;
        memset(e->base, 0xa5, count * PAGE_SIZE);
        return e->base;
    }
    abort();
}
void *alloc_page(void) { return alloc_pages(1); }
void free_page(void *address)
{
    uintptr_t value = (uintptr_t)address;
    for (unsigned i = 0; i < 8; i++) {
        struct extent *e = &extents[i];
        uintptr_t start = (uintptr_t)e->base;
        if (!start || value < start || value - start >= e->pages * PAGE_SIZE) continue;
        assert(!((value - start) % PAGE_SIZE));
        size_t page = (value - start) / PAGE_SIZE;
        assert(!e->released[page]);
        e->released[page] = 1; e->freed++; live_pages--;
        if (e->freed == e->pages) { free(e->base); e->base = NULL; }
        return;
    }
    abort(); /* Includes an attempt to free caller-owned initrd storage. */
}

int main(void)
{
    blkdev_init(); ramdisk_init();
    assert(blkdev_register(BLKDEV_TYPE_RAMDISK, BLKDEV_GEOMETRY_ATA, 1, 512, "bad", NULL, NULL) < 0);
    assert(blkdev_register(BLKDEV_TYPE_AHCI, BLKDEV_GEOMETRY_SOFTWARE, 1, 512, "bad", NULL, NULL) < 0);
    assert(blkdev_register(BLKDEV_TYPE_AHCI, (blkdev_geometry_t)-1, 1, 512, "bad", NULL, NULL) < 0);
    assert(blkdev_register(BLKDEV_TYPE_AHCI, BLKDEV_GEOMETRY_ATA, UINT64_MAX, 512, "bad", NULL, NULL) < 0);
    assert(ramdisk_create(0) < 0 && ramdisk_create(UINT64_MAX) < 0);
    assert(ramdisk_create(SIZE_MAX / BLKDEV_SECTOR_SIZE) < 0);
    assert(!allocation_calls && !live_pages && !blkdev_count());
    fail_allocation = 1;
    assert(ramdisk_create(17) < 0 && !live_pages && !blkdev_count());
    fail_allocation = 0;
    int disk = ramdisk_create(17); /* Three pages, including a partial final page. */
    assert(disk >= 0 && blkdev_get((uint8_t)disk)->sector_count == 17 && live_pages == 3);
    uint8_t *base = ramdisk_get_data((uint8_t)disk);
    for (size_t i = 0; i < 3 * PAGE_SIZE; i++) assert(!base[i]);
    uint8_t data[3 * BLKDEV_SECTOR_SIZE], readback[sizeof(data)];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i ^ (i >> 8));
    assert(!blkdev_write((uint8_t)disk, 7, 3, data)); /* Cross a page boundary. */
    assert(!blkdev_read((uint8_t)disk, 7, 3, readback) && !memcmp(data, readback, sizeof(data)));
    assert(!blkdev_write((uint8_t)disk, 16, 1, data));
    assert(!blkdev_read((uint8_t)disk, 16, 1, readback) && !memcmp(data, readback, 512));
    uint64_t bad_lbas[] = {17, UINT64_MAX, UINT64_MAX - 1};
    for (unsigned i = 0; i < sizeof(bad_lbas) / sizeof(bad_lbas[0]); i++) {
        memset(readback, 0x6d, sizeof(readback));
        assert(blkdev_read((uint8_t)disk, bad_lbas[i], 2, readback) < 0);
        for (size_t j = 0; j < sizeof(readback); j++) assert(readback[j] == 0x6d);
        assert(blkdev_write((uint8_t)disk, bad_lbas[i], 2, data) < 0);
    }
    assert(blkdev_read((uint8_t)disk, 16, 2, readback) < 0);
    assert(blkdev_write((uint8_t)disk, 16, 2, data) < 0);
    assert(blkdev_read((uint8_t)disk, 0, 1, NULL) < 0);
    assert(blkdev_write((uint8_t)disk, 0, 1, NULL) < 0);
    assert(!blkdev_read((uint8_t)disk, 17, 0, NULL));
    assert(!blkdev_write((uint8_t)disk, 17, 0, NULL));
    assert(blkdev_read((uint8_t)disk, 18, 0, NULL) < 0);
    assert(!memcmp(base + 16 * 512, data, 512));

    uint8_t borrowed[1025]; memset(borrowed, 0x83, sizeof(borrowed));
    assert(ramdisk_create_from_buffer(NULL, sizeof(borrowed)) < 0);
    assert(ramdisk_create_from_buffer(borrowed, 511) < 0);
    assert(ramdisk_create_from_buffer((void *)(UINTPTR_MAX - 511), 1024) < 0);
    int initrd = ramdisk_create_from_buffer(borrowed, sizeof(borrowed));
    assert(initrd >= 0 && ramdisk_get_data((uint8_t)initrd) == borrowed && live_pages == 3);
    assert(blkdev_get((uint8_t)initrd)->sector_count == 2);
    assert(!blkdev_read((uint8_t)initrd, 1, 1, readback) && readback[0] == 0x83);
    assert(!blkdev_write((uint8_t)initrd, 1, 1, data) && borrowed[1024] == 0x83);
    assert(blkdev_read((uint8_t)initrd, 2, 1, readback) < 0);

    for (unsigned i = 2; i < BLKDEV_MAX_DEVICES; i++) {
        char name[16]; snprintf(name, sizeof(name), "dummy%u", i);
        assert(blkdev_register(BLKDEV_TYPE_IDE, BLKDEV_GEOMETRY_UNKNOWN, 1, 512, name, NULL, NULL) >= 0);
    }
    assert(ramdisk_create(9) < 0 && live_pages == 3); /* Every allocated page reclaimed. */
    assert(ramdisk_create_from_buffer(borrowed, sizeof(borrowed)) < 0 && live_pages == 3);
    blkdev_init(); /* Test fixture: free dispatcher slots without resetting ramdisk slots. */
    assert(ramdisk_create(1) >= 0 && live_pages == 4); /* Failed registrations did not consume a slot. */
    assert(ramdisk_create_from_buffer(borrowed, sizeof(borrowed)) >= 0);
    size_t calls = allocation_calls;
    assert(ramdisk_create(1) < 0 && allocation_calls == calls);
    int devices = blkdev_count();
    assert(!blkdev_seal() && blkdev_is_sealed() && blkdev_seal() == -16);
    blkdev_unregister(0); blkdev_init();
    assert(blkdev_count() == devices);
    assert(blkdev_register(BLKDEV_TYPE_IDE, BLKDEV_GEOMETRY_UNKNOWN, 1, 512, "late", NULL, NULL) < 0);
    for (unsigned i = 0; i < 8; i++) if (extents[i].base) {
        uint8_t *start = extents[i].base; size_t pages = extents[i].pages;
        for (size_t page = 0; page < pages; page++) free_page(start + page * PAGE_SIZE);
    }
    assert(!live_pages);
    puts("ramdisk tests passed: full zeroing, page crossings, overflow, OOM, registration rollback, borrowed buffer");
    return 0;
}
