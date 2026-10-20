/* Exercise production VFS fallback I/O; unrelated device paths must stay unused. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vfs.h"
#include "memory.h"

process_t *process_current(void) { return NULL; }
void spinlock_acquire(spinlock_t *lock) { (void)lock; }
void spinlock_release(spinlock_t *lock) { (void)lock; }
void *alloc_page(void) { return calloc(1, PAGE_SIZE); }
int chardev_read(vfs_file_t *f, void *b, size_t n) { (void)f; (void)b; (void)n; abort(); }
int chardev_write(vfs_file_t *f, const void *b, size_t n) { (void)f; (void)b; (void)n; abort(); }
int net_recv(int fd, void *b, size_t n, int flags) { (void)fd; (void)b; (void)n; (void)flags; abort(); }
int net_send(int fd, const void *b, size_t n, int flags) { (void)fd; (void)b; (void)n; (void)flags; abort(); }
void serial_puts(const char *s) { (void)s; }
void serial_put_dec(uint64_t n) { (void)n; }

static void read_bounds(vfs_file_t *file)
{
    unsigned char out[8];
    const uint64_t offsets[] = {3, 4, PAGE_SIZE + 1, UINT64_MAX};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        memset(out, 0xa5, sizeof(out));
        file->offset = offsets[i];
        assert(vfs_read(3, out, sizeof(out)) == 0);
        assert(file->offset == offsets[i]);
        for (unsigned j = 0; j < sizeof(out); j++) assert(out[j] == 0xa5);
    }
    file->offset = 1;
    assert(vfs_read(3, NULL, 0) == 0 && file->offset == 1);
    assert(vfs_read(3, out, sizeof(out)) == 2 && !memcmp(out, "bc", 2));
    assert(file->offset == 3);
    file->inode->size = 0;
    file->offset = 0;
    assert(vfs_read(3, out, sizeof(out)) == 0 && file->offset == 0);
    file->inode->size = 3;
    puts("VFS EOF bounds passed");
}

static void write_bounds(vfs_file_t *file)
{
    unsigned char *data = file->inode->fs_data;
    file->offset = 1;
    assert(vfs_write(3, NULL, 0) == 0 && file->offset == 1 && file->inode->size == 3);
    const uint64_t offsets[] = {PAGE_SIZE, PAGE_SIZE + 1, UINT64_MAX};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        file->offset = offsets[i];
        assert(vfs_write(3, "x", 1) == 0);
        assert(file->offset == offsets[i] && file->inode->size == 3);
        assert(!memcmp(data, "abc", 3));
    }
    file->offset = PAGE_SIZE - 1;
    /* Huge count must be clamped before offset + count can wrap. */
    assert(vfs_write(3, "z", SIZE_MAX) == 1);
    assert(file->offset == PAGE_SIZE && file->inode->size == PAGE_SIZE);
    assert(data[PAGE_SIZE - 1] == 'z');
    file->offset = 1;
    assert(vfs_write(3, "XY", 2) == 2 && file->offset == 3);
    assert(!memcmp(data, "aXY", 3) && file->inode->size == PAGE_SIZE);
    puts("VFS single-page write bounds passed");
}

int main(int argc, char **argv)
{
    unsigned char *data = calloc(1, PAGE_SIZE);
    assert(data);
    memcpy(data, "abc", 3);
    vfs_inode_t inode = {.type = VFS_TYPE_FILE, .size = 3, .fs_data = data};
    vfs_file_t file = {.inode = &inode};
    vfs_get_fd_table()[3] = &file;
    if (argc == 1 || !strcmp(argv[1], "read")) read_bounds(&file);
    if (argc == 1 || !strcmp(argv[1], "write")) write_bounds(&file);
    vfs_get_fd_table()[3] = NULL;
    free(data);
    return 0;
}
