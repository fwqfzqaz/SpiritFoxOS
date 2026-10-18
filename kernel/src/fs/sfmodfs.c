/* 兼容桥：/modules 的普通文件操作经消息进入 fs.vfs -> fs.tmpfs。
 * 目录项沿用现有 VFS；数据归模块所有。仅支持平面目录，非完整 POSIX tmpfs。
 */
#include "../../core/internal.h"
#include "vfs.h"
#include "memory.h"
#include "string.h"

static uint64_t service;

/* 一次请求至多 4 KiB，返回短读/写，调用者可继续传输。 */
static int transfer(vfs_file_t *file, void *buffer, size_t count, int writing)
{
    if (!file || !file->inode || (!buffer && count)) return SFM_EINVAL;
    if (!count) return 0;
    if (count > SFM_PAYLOAD) count = SFM_PAYLOAD;
    sfm_message_t *request = sf_alloc(sizeof(*request));
    sfm_message_t *response = sf_alloc(sizeof(*response));
    int result = SFM_ENOMEM;
    if (!request || !response) goto done;
    sfm_clear(request, sizeof(*request));
    request->type = writing ? SFM_FILE_WRITE : SFM_FILE_READ;
    request->handle = file->inode->ino;
    request->offset = file->offset;
    uint32_t length = (uint32_t)count;
    request->size = writing ? length : sizeof(length);
    sfm_copy(request->data, writing ? buffer : &length, request->size);
    result = sfm_call(service, request, response);
    if (!result)
    {
        if (response->result < 0 || (uint64_t)response->result > count ||
            (!writing && response->size != (uint32_t)response->result))
        { result = -5; goto done; }
        result = (int)response->result;
        if (!writing) sfm_copy(buffer, response->data, (size_t)result);
        file->offset += (size_t)result;
        if (writing && file->offset > file->inode->size) file->inode->size = file->offset;
    }
done:
    sf_free(request);
    sf_free(response);
    return result;
}

static int read_file(vfs_file_t *file, void *buffer, size_t count)
{ return transfer(file, buffer, count, 0); }
static int write_file(vfs_file_t *file, const void *buffer, size_t count)
{ return transfer(file, (void *)buffer, count, 1); }

static int control(vfs_inode_t *inode, uint32_t operation, uint64_t offset)
{
    sfm_message_t *request = sf_alloc(sizeof(*request));
    sfm_message_t *response = sf_alloc(sizeof(*response));
    int result = SFM_ENOMEM;
    if (request && response)
    {
        sfm_clear(request, sizeof(*request));
        request->type = operation;
        request->handle = inode->ino;
        request->offset = offset;
        result = sfm_call(service, request, response);
    }
    sf_free(request);
    sf_free(response);
    return result;
}

static int truncate_file(vfs_inode_t *inode, uint64_t size)
{
    int result = control(inode, SFM_FILE_TRUNCATE, size);
    if (!result) inode->size = size;
    return result;
}

static vfs_inode_t *allocate_inode(vfs_superblock_t *sb)
{
    vfs_inode_t *inode = alloc_page();
    if (!inode) return NULL;
    memset(inode, 0, PAGE_SIZE);
    inode->sb = sb;
    inode->nlinks = 1;
    inode->read = read_file;
    inode->write = write_file;
    inode->truncate = truncate_file;
    return inode;
}

static void destroy_inode(vfs_inode_t *inode) { free_page(inode); }

static int create_file(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{
    if (parent->inode != parent->inode->sb->root_inode) return SFM_EINVAL;
    size_t length = 0;
    while (length < 32 && name[length]) length++;
    if (!length || length >= 32) return -63;
    sfm_message_t *request = sf_alloc(sizeof(*request));
    sfm_message_t *response = sf_alloc(sizeof(*response));
    int result = SFM_ENOMEM;
    if (request && response)
    {
        sfm_clear(request, sizeof(*request));
        request->type = SFM_FILE_OPEN;
        request->flags = SFM_CREATE;
        request->size = 9 + length + 1;
        sfm_copy(request->data, "/modules/", 9);
        sfm_copy(request->data + 9, name, length + 1);
        result = sfm_call(service, request, response);
        if (!result) inode->ino = response->handle;
    }
    sf_free(request);
    sf_free(response);
    return result;
}

static int make_directory(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{ (void)parent; (void)name; (void)inode; return SFM_ENOSYS; }
static int unlink_file(vfs_dentry_t *entry)
{ return control(entry->inode, SFM_FILE_REMOVE, 0); }

static int mount_fs(vfs_superblock_t *sb, uint8_t device, const char *options)
{
    (void)device; (void)options;
    vfs_inode_t *root = allocate_inode(sb);
    if (!root) return SFM_ENOMEM;
    vfs_dentry_t *dentry = alloc_page();
    if (!dentry) { free_page(root); return SFM_ENOMEM; }
    memset(dentry, 0, PAGE_SIZE);
    root->type = VFS_TYPE_DIR;
    root->mode = VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR;
    root->read = NULL;
    root->write = NULL;
    root->truncate = NULL;
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
    .create = create_file, .mkdir = make_directory, .unlink = unlink_file
};

int sfm_mount_compat(void)
{
    service = sfm_lookup("fs.vfs");
    if (!service || vfs_register_fs(&filesystem)) return -1;
    if (vfs_mkdir("/modules", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR) ||
        vfs_mount(NULL, "/modules", "sfmodfs", 0, NULL)) return -1;
    int fd = vfs_open("/modules/README", VFS_O_CREAT | VFS_O_RDWR, VFS_S_IRUSR | VFS_S_IWUSR);
    if (fd < 0) return -1;
    const char text[] = "This file is stored by fs.tmpfs.sfmod via fs.vfs messages.\n";
    char result[sizeof(text)];
    int passed = vfs_write(fd, text, sizeof(text) - 1) == sizeof(text) - 1 &&
        vfs_seek(fd, 0, VFS_SEEK_SET) == 0 &&
        vfs_read(fd, result, sizeof(result)) == sizeof(text) - 1;
    vfs_close(fd);
    if (!passed || memcmp(text, result, sizeof(text) - 1)) return -1;
    sf_log("[sfmod] VFS bridge passed: /modules/README\n");
    return 0;
}
