/* Import actual boot nodes; both public and message enumeration then use the registry. */
#include "../../core/internal.h"
#include "devfs.h"
#include "chardev.h"
#include "blkdev.h"
#include "string.h"
extern int sfm_prepare_devices(void);
extern int sfm_devfs_readdir(vfs_file_t *, int, char *, int, vfs_inode_t *);
static sfm_message_t request, response;
static uint64_t registry;
static uint32_t imported;
static struct binding { vfs_dentry_t *entry; unsigned depth; char path[256]; } bindings[SFM_DEV_MAX];

static int import_node(vfs_dentry_t *entry, const char *path, unsigned depth)
{
    if (depth > 8 || !entry || !entry->inode || imported >= SFM_DEV_MAX) return -22;
    vfs_inode_t *inode = entry->inode;
    sfm_device_record_t item;
    sfm_clear(&item, sizeof(item));
    item.version = SFM_DEV_VERSION; item.size = sizeof(item); item.owner = SFM_DEV_BOOT;
    if (strlen(path) >= sizeof(item.path)) return -36;
    strcpy(item.path, path);
    if (inode->type == VFS_TYPE_DIR) { item.type = SFM_NODE_DIR; item.mode = 0040000; }
    else if (inode->type == VFS_TYPE_CHARDEV) {
        item.type = SFM_NODE_CHARDEV; item.mode = 0020000;
        if (inode->open && inode->chardev_id == UINT32_MAX) {
            item.owner = SFM_DEV_SPECIAL; item.backend = SFM_DEV_TMPFS;
            item.backend_handle = inode->ino; item.rdev = inode->rdev; item.flags = SFM_DEV_RDEV_KNOWN;
        } else if (inode->chardev_id == 1) {
            item.backend = SFM_DEV_CONSOLE; item.backend_handle = 1;
        } else {
            if (!chardev_get_ops(inode->chardev_id)) return -19;
            item.backend = SFM_DEV_CHAR; item.backend_handle = inode->chardev_id;
        }
    } else if (inode->type == VFS_TYPE_BLKDEV) {
        blkdev_t *dev = blkdev_get(inode->blkdev_id);
        if (!dev || !dev->in_use) return -19;
        item.type = SFM_NODE_BLKDEV; item.mode = 0060000;
        item.backend = SFM_DEV_BLOCK; item.backend_handle = inode->blkdev_id; item.bytes = inode->size;
    } else return -95;
    item.mode |= inode->mode & 07777;
    sfm_clear(&request, sizeof(request)); request.flags = SFM_DEV_VERSION;
    request.type = imported ? SFM_DEV_REGISTER : SFM_FS_ROOT;
    if (imported) { request.size = sizeof(item); sfm_copy(request.data, &item, sizeof(item)); }
    if (sfm_call(registry, &request, &response)) return -5;
    inode->ino = response.handle; inode->mode = item.mode; inode->nlinks = 1;
    if (inode->type == VFS_TYPE_DIR) {
        inode->fs_data = (void *)(uintptr_t)SFM_ROUTE_DEVFS;
        inode->readdir = sfm_devfs_readdir;
    }
    imported++;
    return 0;
}
static int compare_node(vfs_dentry_t *entry, const char *path, unsigned depth)
{
    if (depth > 8) return -22;
    sfm_clear(&request, sizeof(request)); request.type = SFM_DEV_LOOKUP; request.flags = SFM_DEV_VERSION;
    request.size = (uint32_t)strlen(path) + 1; sfm_copy(request.data, path, request.size);
    if (sfm_call(registry, &request, &response) || response.result != 1 ||
        response.size != sizeof(sfm_device_record_t)) return -5;
    sfm_device_record_t item; sfm_copy(&item, response.data, sizeof(item));
    vfs_inode_t *inode = entry->inode;
    if (item.handle != inode->ino || item.mode != inode->mode || item.rdev != inode->rdev) return -5;
    if (inode->type == VFS_TYPE_DIR) {
        int fd = vfs_open(path, VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
        if (fd < 0) return -5;
        unsigned expected = 0, actual = 0;
        for (vfs_dentry_t *child = entry->child; child; child = child->next) expected++;
        uint64_t seen = 0;
        for (;;) {
            char name[VFS_MAX_NAME]; vfs_inode_t stat;
            int result = vfs_readdir(fd, (int)actual, name, sizeof(name), &stat);
            if (result <= 0) { if (result < 0) { vfs_close(fd); return -5; } break; }
            int found = -1;
            for (uint32_t i = 0; i < imported; i++)
                if (bindings[i].entry->parent == entry && bindings[i].entry != entry &&
                    !strcmp(bindings[i].entry->name, name) && bindings[i].entry->inode->ino == stat.ino)
                    { found = (int)i; break; }
            if (found < 0 || (seen & (1ull << found))) { vfs_close(fd); return -5; }
            seen |= 1ull << found;
            if (++actual > SFM_DEV_MAX) { vfs_close(fd); return -5; }
        }
        vfs_close(fd);
        if (expected != actual) return -5;
    }
    return 0;
}
int sfm_devices_boot(void)
{
    registry = sfm_lookup("dev.registry");
    if (!registry || sfm_prepare_devices()) return -1;
    imported = 0;
    uint32_t queued = 1;
    bindings[0].entry = devfs_get_root(); bindings[0].depth = 0;
    strcpy(bindings[0].path, "/dev");
    for (uint32_t i = 0; i < queued; i++) {
        struct binding *b = &bindings[i];
        if (import_node(b->entry, b->path, b->depth)) return -1;
        for (vfs_dentry_t *child = b->entry->child; child; child = child->next) {
            if (queued == SFM_DEV_MAX) return -1;
            struct binding *next = &bindings[queued++];
            size_t n = strlen(b->path), c = strlen(child->name);
            if (n + c + 2 > sizeof(next->path)) return -1;
            next->entry = child; next->depth = b->depth + 1;
            memcpy(next->path, b->path, n); next->path[n++] = '/';
            memcpy(next->path + n, child->name, c + 1);
        }
    }
    sfm_clear(&request, sizeof(request)); request.type = SFM_DEV_SEAL; request.flags = SFM_DEV_VERSION;
    if (sfm_call(registry, &request, &response)) return -1;
    devfs_seal();
    for (uint32_t i = 0; i < imported; i++)
        if (compare_node(bindings[i].entry, bindings[i].path, bindings[i].depth)) return -1;
    sfm_clear(&request, sizeof(request)); request.type = SFM_DEV_ENUM;
    request.flags = SFM_DEV_VERSION; request.offset = imported;
    if (sfm_call(registry, &request, &response) || response.result || sfm_device_selftest()) return -1;
    sf_log("[sfmod] special devices attached: /dev/null /dev/zero\n");
    sf_log("[sfmod] unified device registry matches public /dev\n");
    return 0;
}
