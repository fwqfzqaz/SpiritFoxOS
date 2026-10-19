/* 普通文件调用的兼容桥；/modules、/tmp、/run 的数据与树节点归模块所有。 */
#include "../../core/internal.h"
#include "vfs.h"
#include "memory.h"
#include "string.h"
#include "devfs.h"

static uint64_t service;
/* 当前 RPC 不阻塞；短临界区串行复用缓冲区，使 close/unlink 不依赖临时分配。 */
static sfm_message_t request, response;
static unsigned char rpc_lock;
static uint64_t inode_route(const vfs_inode_t *inode)
{ return (uint64_t)(uintptr_t)inode->fs_data; }

static uint64_t begin(void)
{
    uint64_t flags = sf_irq_save();
    while (__atomic_test_and_set(&rpc_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    sfm_clear(&request, sizeof(request));
    return flags;
}
static void end(uint64_t flags)
{
    __atomic_clear(&rpc_lock, __ATOMIC_RELEASE);
    sf_irq_restore(flags);
}

static int control(vfs_inode_t *inode, uint32_t operation, uint64_t offset)
{
    uint64_t flags = begin();
    request.type = operation;
    request.handle = inode->ino;
    request.route = inode_route(inode);
    request.offset = offset;
    int result = sfm_call(service, &request, &response);
    end(flags);
    return result;
}

static int open_file(vfs_file_t *file)
{
    if ((file->flags & 3u) == 3u) return SFM_EINVAL;
    if ((file->flags & VFS_O_DIRECTORY) && file->inode->type != VFS_TYPE_DIR) return -20;
    if (file->inode->type == VFS_TYPE_DIR && (file->flags & (VFS_O_WRONLY | VFS_O_RDWR)))
        return -21;
    int result = control(file->inode, SFM_FILE_RETAIN, 0);
    if (result) return result;
    if ((file->flags & VFS_O_TRUNC) && file->inode->type == VFS_TYPE_FILE &&
        (file->flags & (VFS_O_WRONLY | VFS_O_RDWR)))
    {
        result = control(file->inode, SFM_FILE_TRUNCATE, 0);
        if (result) { control(file->inode, SFM_FILE_RELEASE, 0); return result; }
        file->inode->size = 0;
    }
    return 0;
}

static void release_file(vfs_file_t *file)
{
    if (control(file->inode, SFM_FILE_RELEASE, 0))
        sf_log("[sfmod] bridge release failed\n");
}

/* 单次 4 KiB；保留短读写语义，调用方可以继续传输。 */
static int transfer(vfs_file_t *file, void *buffer, size_t count, int writing)
{
    if (!file || !file->inode || (!buffer && count)) return SFM_EINVAL;
    if (file->inode->type == VFS_TYPE_DIR) return -21;
    uint32_t access = file->flags & 3u;
    if ((!writing && access == VFS_O_WRONLY) || (writing && access == VFS_O_RDONLY)) return -9;
    if (!count) return 0;
    if (count > SFM_PAYLOAD) count = SFM_PAYLOAD;
    uint64_t flags = begin();
    int result = 0;
    if (writing && file->inode->type == VFS_TYPE_FILE && (file->flags & VFS_O_APPEND))
    {
        request.type = SFM_FILE_STAT;
        request.handle = file->inode->ino;
        request.route = inode_route(file->inode);
        result = sfm_call(service, &request, &response);
        if (result || response.result < 0) goto done;
        file->offset = (uint64_t)response.result;
    }
    request.type = writing ? SFM_FILE_WRITE : SFM_FILE_READ;
    request.handle = file->inode->ino;
    request.route = inode_route(file->inode);
    request.offset = file->offset;
    uint32_t length = (uint32_t)count;
    request.size = writing ? length : sizeof(length);
    sfm_copy(request.data, writing ? buffer : &length, request.size);
    result = sfm_call(service, &request, &response);
    if (!result)
    {
        if (response.result < 0 || (uint64_t)response.result > count ||
            (!writing && response.size != (uint32_t)response.result))
        { result = -5; goto done; }
        result = (int)response.result;
        if (!writing) sfm_copy(buffer, response.data, (size_t)result);
        if (file->inode->type == VFS_TYPE_FILE) {
            file->offset += (size_t)result;
            if (writing && file->offset > file->inode->size) file->inode->size = file->offset;
        }
    }
done:
    end(flags);
    return result;
}
static int read_file(vfs_file_t *file, void *buffer, size_t count)
{ return transfer(file, buffer, count, 0); }
static int write_file(vfs_file_t *file, const void *buffer, size_t count)
{ return transfer(file, (void *)buffer, count, 1); }

static int64_t seek_device(vfs_file_t *file, int64_t offset, int whence)
{
    if (whence < 0 || whence > 2) return SFM_EINVAL;
    uint64_t flags = begin();
    request.type = SFM_FILE_SEEK;
    request.handle = file->inode->ino;
    request.route = inode_route(file->inode);
    request.offset = (uint64_t)offset;
    request.flags = (uint32_t)whence;
    int result = sfm_call(service, &request, &response);
    int64_t position = result ? result : response.result;
    if (position >= 0) file->offset = (uint64_t)position;
    end(flags);
    return position;
}

static int truncate_file(vfs_inode_t *inode, uint64_t size)
{
    int result = control(inode, SFM_FILE_TRUNCATE, size);
    if (!result) inode->size = size;
    return result;
}

static int read_directory(vfs_file_t *file, int index, char *name, int namelen, vfs_inode_t *stat)
{
    if (index < 0 || !name || namelen < 1) return SFM_EINVAL;
    uint64_t flags = begin();
    request.type = SFM_FILE_READDIR;
    request.handle = file->inode->ino;
    request.route = inode_route(file->inode);
    request.offset = (uint64_t)index;
    int result = sfm_call(service, &request, &response);
    if (!result && response.result)
    {
        if (!response.size || response.size > SFM_FILE_NAME || response.data[response.size - 1])
        { result = -5; goto done; }
        size_t count = response.size;
        if (count > (size_t)namelen) count = (size_t)namelen;
        sfm_copy(name, response.data, count);
        name[count - 1] = 0;
        if (stat)
        {
            memset(stat, 0, sizeof(*stat));
            stat->ino = response.handle;
            stat->type = response.flags == SFM_NODE_DIR ? VFS_TYPE_DIR :
                response.flags == SFM_NODE_SYMLINK ? VFS_TYPE_SYMLINK :
                response.flags == SFM_NODE_CHARDEV ? VFS_TYPE_CHARDEV :
                response.flags == SFM_NODE_BLKDEV ? VFS_TYPE_BLKDEV : VFS_TYPE_FILE;
            stat->size = stat->type == VFS_TYPE_CHARDEV ? 0 : response.offset;
            if (stat->type == VFS_TYPE_CHARDEV) stat->rdev = response.offset;
            stat->nlinks = 1;
            stat->mode = stat->type == VFS_TYPE_DIR ? 0700 : 0600;
        }
        result = 1;
    }
done:
    end(flags);
    return result;
}

static vfs_inode_t *allocate_inode(vfs_superblock_t *sb)
{
    vfs_inode_t *inode = alloc_page();
    if (!inode) return NULL;
    memset(inode, 0, PAGE_SIZE);
    inode->sb = sb;
    inode->nlinks = 1;
    inode->open = open_file;
    inode->release = release_file;
    inode->read = read_file;
    inode->write = write_file;
    inode->truncate = truncate_file;
    inode->readdir = read_directory;
    return inode;
}
static void destroy_inode(vfs_inode_t *inode) { free_page(inode); }

static int create_node(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode, int directory)
{
    size_t length = 0;
    while (length < SFM_FILE_NAME && name[length]) length++;
    if (!length || length >= SFM_FILE_NAME) return -36;
    uint64_t flags = begin();
    request.type = directory ? SFM_FILE_MKDIR : SFM_FILE_OPEN;
    request.flags = SFM_CREATE;
    request.handle = parent->inode->ino;
    request.route = inode_route(parent->inode);
    request.size = (uint32_t)length + 1;
    sfm_copy(request.data, name, request.size);
    int result = sfm_call(service, &request, &response);
    if (!result && response.flags != (directory ? SFM_NODE_DIR : SFM_NODE_FILE)) result = -17;
    if (!result) {
        inode->ino = response.handle;
        inode->fs_data = (void *)(uintptr_t)response.route;
    }
    end(flags);
    return result;
}
static int create_file(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{ return create_node(parent, name, inode, 0); }
static int make_directory(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{ return create_node(parent, name, inode, 1); }
static int unlink_file(vfs_dentry_t *entry)
{ return control(entry->inode, SFM_FILE_REMOVE, 0); }

static int create_symlink(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{
    size_t length = strlen(name), target = strlen(inode->symlink_target);
    if (!length || length >= SFM_FILE_NAME || !target || target >= SFM_FILE_PATH) return -36;
    uint64_t flags = begin();
    request.type = SFM_FILE_SYMLINK;
    request.handle = parent->inode->ino;
    request.route = inode_route(parent->inode);
    request.size = (uint32_t)(length + target + 2);
    sfm_copy(request.data, name, length + 1);
    sfm_copy(request.data + length + 1, inode->symlink_target, target + 1);
    int result = sfm_call(service, &request, &response);
    if (!result) {
        inode->ino = response.handle;
        inode->fs_data = (void *)(uintptr_t)response.route;
    }
    end(flags);
    return result;
}

static int mount_fs(vfs_superblock_t *sb, uint8_t device, const char *path)
{
    (void)device;
    if (!path || strlen(path) >= SFM_FILE_PATH) return SFM_EINVAL;
    vfs_inode_t *root = allocate_inode(sb);
    if (!root) return SFM_ENOMEM;
    vfs_dentry_t *dentry = alloc_page();
    if (!dentry) { free_page(root); return SFM_ENOMEM; }
    uint64_t flags = begin();
    request.type = SFM_FILE_OPEN;
    request.size = (uint32_t)strlen(path) + 1;
    sfm_copy(request.data, path, request.size);
    int result = sfm_call(service, &request, &response);
    if (!result && response.flags != SFM_NODE_DIR) result = -20;
    root->ino = response.handle;
    root->fs_data = (void *)(uintptr_t)response.route;
    end(flags);
    if (result) { free_page(dentry); free_page(root); return result; }
    memset(dentry, 0, PAGE_SIZE);
    root->type = VFS_TYPE_DIR;
    root->mode = 0700;
    strcpy(dentry->name, "/");
    dentry->inode = root;
    dentry->parent = dentry;
    sb->root_inode = root;
    sb->root = dentry;
    return 0;
}
static int unmount_fs(vfs_superblock_t *sb) { (void)sb; return SFM_EBUSY; }
static vfs_filesystem_t filesystem = {
    .name = "sfmodfs", .mount = mount_fs, .unmount = unmount_fs,
    .alloc_inode = allocate_inode, .destroy_inode = destroy_inode,
    .create = create_file, .mkdir = make_directory, .unlink = unlink_file, .symlink = create_symlink
};

int sfm_prepare_devices(void)
{
    uint64_t backend = sfm_lookup("dev.tmpfs"), root;
    uint64_t saved = begin();
    request.type = SFM_FS_ROOT;
    int status = sfm_call(backend, &request, &response);
    root = response.handle;
    end(saved);
    if (status) return status;
    static const char *names[] = {"null", "zero"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        uint64_t flags = begin();
        request.type = SFM_FILE_OPEN;
        request.handle = root;
        request.size = (uint32_t)strlen(names[i]) + 1u;
        sfm_copy(request.data, names[i], request.size);
        int result = sfm_call(backend, &request, &response);
        if (!result && (response.flags != SFM_NODE_CHARDEV || !response.handle)) result = -5;
        vfs_inode_t *inode = result ? NULL : devfs_boot_char_node(names[i]);
        if (inode) {
            inode->ino = response.handle;
            inode->fs_data = (void *)(uintptr_t)SFM_ROUTE_DEVFS;
            inode->rdev = response.offset;
            inode->type = VFS_TYPE_CHARDEV;
            inode->mode = 0020000u | 0666u;
            inode->nlinks = 1;
            inode->chardev_id = UINT32_MAX; /* No legacy dispatcher slot. */
            inode->open = open_file;
            inode->release = release_file;
            inode->read = read_file;
            inode->write = write_file;
            inode->truncate = truncate_file;
            inode->seek = seek_device;
        }
        end(flags);
        if (result || !inode) return -1;
    }
    return 0;
}

int sfm_devfs_readdir(vfs_file_t *file, int index, char *name, int size, vfs_inode_t *stat)
{ return read_directory(file, index, name, size, stat); }

int sfm_mount_compat(void)
{
    service = sfm_lookup("fs.vfs");
    if (!service || vfs_register_fs(&filesystem)) return -1;
    static const char *paths[] = {"/modules", "/tmp", "/run"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        if (vfs_mkdir(paths[i], 0700) || vfs_mount(NULL, paths[i], "sfmodfs", 0, paths[i]))
            return -1;
    int fd = vfs_open("/modules/README", VFS_O_CREAT | VFS_O_RDWR, 0600);
    if (fd < 0) return -1;
    const char text[] = "This file is stored by fs.tmpfs.sfmod via fs.vfs messages.\n";
    char result[sizeof(text)];
    int passed = vfs_write(fd, text, sizeof(text) - 1) == sizeof(text) - 1 &&
        vfs_seek(fd, 0, VFS_SEEK_SET) == 0 &&
        vfs_read(fd, result, sizeof(result)) == sizeof(text) - 1;
    vfs_close(fd);
    if (!passed || memcmp(text, result, sizeof(text) - 1)) return -1;
    sf_log("[sfmod] VFS bridge passed: /modules/README\n");
    /* 保留旧 VFS 的符号链接调用路径，并验证 unlink 不会删除目标。 */
    if (vfs_mkdir("/tmp/link-test", 0700) ||
        vfs_symlink("/modules/README", "/tmp/link-test/readme")) return -1;
    char target[32];
    if (vfs_readlink("/tmp/link-test/readme", target, sizeof(target)) != 15 ||
        memcmp(target, "/modules/README", 15)) return -1;
    fd = vfs_open("/tmp/link-test/readme", VFS_O_RDONLY, 0);
    if (fd < 0 || vfs_read(fd, result, 4) != 4 || memcmp(result, "This", 4) || vfs_close(fd) ||
        vfs_unlink("/tmp/link-test/readme") || !vfs_resolve_path("/modules/README") ||
        vfs_rmdir("/tmp/link-test") || vfs_unmount("/tmp") != SFM_EBUSY) return -1;
    sf_log("[sfmod] tmpfs mounted: /tmp /run (independent roots)\n");
    return 0;
}
