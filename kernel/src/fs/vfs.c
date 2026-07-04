#include "vfs.h"
#include "memfs.h"
#include "string.h"
#include "memory.h"
#include "vga.h"
#include "process.h"

/* ========================================================================
 * Global VFS state
 * ======================================================================== */
static int vfs_debug_resolve = 0;  /* Set to 1 to enable VFS_RESOLVE debug logs */

/* Registered filesystem types */
#define VFS_MAX_FS_TYPES 8
static vfs_filesystem_t *fs_types[VFS_MAX_FS_TYPES];
static int fs_type_count = 0;

/* Mount points */
static vfs_mount_t mounts[VFS_MAX_MOUNTS];
static int mount_count = 0;

/* Root dentry */
static vfs_dentry_t *root_dentry = NULL;
static vfs_inode_t  *root_inode  = NULL;

/* Current working directory */
static char cwd[VFS_MAX_PATH] = "/";

/* File descriptor table (fallback for early boot before process_init) */
static vfs_file_t *fd_table[VFS_MAX_FD];

/* Get the active fd table - prefer per-process if available */
vfs_file_t **vfs_get_fd_table(void)
{
    process_t *proc = process_current();
    if (proc) {
        return proc->fd_table;
    }
    return fd_table;
}

/* ========================================================================
 * Dentry helpers
 * ======================================================================== */

static vfs_dentry_t *dentry_alloc(const char *name, vfs_inode_t *inode)
{
    vfs_dentry_t *d = (vfs_dentry_t *)alloc_page();
    if (!d) return NULL;
    memset(d, 0, PAGE_SIZE);
    strncpy(d->name, name, VFS_MAX_NAME - 1);
    d->inode = inode;
    d->parent = NULL;
    d->child = NULL;
    d->next = NULL;
    d->mounted = 0;
    d->mount_root = NULL;
    return d;
}

static void dentry_add_child(vfs_dentry_t *parent, vfs_dentry_t *child)
{
    child->parent = parent;
    /* Add to beginning of child list */
    child->next = parent->child;
    parent->child = child;
}

static vfs_dentry_t *dentry_lookup(vfs_dentry_t *parent, const char *name)
{
    vfs_dentry_t *d = parent->child;
    while (d) {
        if (strcmp(d->name, name) == 0)
            return d;
        d = d->next;
    }
    return NULL;
}

/* ========================================================================
 * Path resolution
 * ======================================================================== */

/* Maximum symlink recursion depth */
#define VFS_MAX_SYMLINK_DEPTH 8

/* Split a path into components. Returns number of components.
 * Components are stored in the provided array. */
static int path_split(const char *path, char components[][VFS_MAX_NAME], int max_components)
{
    int count = 0;
    int i = 0;

    /* Skip leading slashes */
    while (path[i] == '/')
        i++;

    while (path[i] && count < max_components) {
        int j = 0;
        while (path[i] && path[i] != '/' && j < VFS_MAX_NAME - 1) {
            components[count][j++] = path[i++];
        }
        components[count][j] = '\0';

        if (j > 0) {
            /* Skip "." */
            if (strcmp(components[count], ".") != 0)
                count++;
        }

        while (path[i] == '/')
            i++;
    }

    return count;
}

/* Internal path resolution with symlink following support.
 * depth: current symlink recursion depth
 * follow_last: if 0, don't follow symlink on the final path component
 *              (used by readlink); if 1, follow all symlinks */
static vfs_dentry_t *vfs_resolve_dentry_internal(const char *path, int depth, int follow_last)
{
    if (!root_dentry) return NULL;
    if (depth > VFS_MAX_SYMLINK_DEPTH) return NULL;  /* Symlink loop */

    char components[32][VFS_MAX_NAME];
    int count;

    /* Handle absolute vs relative paths */
    char full_path[VFS_MAX_PATH];
    if (path[0] == '/') {
        strncpy(full_path, path, VFS_MAX_PATH - 1);
        full_path[VFS_MAX_PATH - 1] = '\0';
    } else {
        /* Relative to CWD - simple concatenation */
        int cwd_len = strlen(cwd);
        int path_len = strlen(path);
        if (cwd_len + 1 + path_len >= VFS_MAX_PATH)
            return NULL;
        strcpy(full_path, cwd);
        if (cwd[cwd_len - 1] != '/') {
            full_path[cwd_len] = '/';
            cwd_len++;
        }
        strcpy(full_path + cwd_len, path);
    }

    /* Handle special case: root */
    if (strcmp(full_path, "/") == 0)
        return root_dentry;

    count = path_split(full_path, components, 32);
    if (count == 0) return root_dentry;

    /* Walk the dentry tree */
    vfs_dentry_t *current = root_dentry;
    if (vfs_debug_resolve) printf("[VFS_RESOLVE] resolving '%s', %d components\n", full_path, count);

    for (int i = 0; i < count; i++) {
        /* Handle ".." */
        if (strcmp(components[i], "..") == 0) {
            if (current->parent)
                current = current->parent;
            continue;
        }

        /* Check mount point */
        if (current->mounted && current->mount_root) {
            current = current->mount_root;
        }

        /* Look up child */
        if (vfs_debug_resolve) printf("[VFS_RESOLVE] looking up '%s' in dentry '%s' (mounted=%d)\n", components[i], current->name ? current->name : "?", current->mounted);
        vfs_dentry_t *child = dentry_lookup(current, components[i]);
        if (!child) {
            if (vfs_debug_resolve) printf("[VFS_RESOLVE] component '%s' NOT FOUND in '%s'\n", components[i], current->name ? current->name : "?");
            return NULL;  /* Not found */
        }
        if (vfs_debug_resolve) printf("[VFS_RESOLVE] found '%s' at %p (mounted=%d)\n", components[i], (void *)child, child->mounted);

        /* Check if this component is a symlink that should be followed */
        int is_last = (i == count - 1);
        if (child->inode && child->inode->type == VFS_TYPE_SYMLINK &&
            child->inode->symlink_target[0] != '\0' &&
            (follow_last || !is_last)) {
            /* Construct new path from symlink target + remaining components */
            char new_path[VFS_MAX_PATH];
            int new_len = 0;

            if (child->inode->symlink_target[0] == '/') {
                /* Absolute symlink - use target directly */
                int tgt_len = strlen(child->inode->symlink_target);
                if (tgt_len >= VFS_MAX_PATH) return NULL;
                memcpy(new_path, child->inode->symlink_target, tgt_len);
                new_len = tgt_len;
            } else {
                /* Relative symlink - build path from parent directory */
                new_path[0] = '/';
                new_len = 1;
                for (int j = 0; j < i; j++) {
                    if (new_len > 1) {
                        if (new_len >= VFS_MAX_PATH - 1) return NULL;
                        new_path[new_len++] = '/';
                    }
                    int cplen = strlen(components[j]);
                    if (new_len + cplen >= VFS_MAX_PATH) return NULL;
                    memcpy(new_path + new_len, components[j], cplen);
                    new_len += cplen;
                }
                if (new_len > 1) {
                    if (new_len >= VFS_MAX_PATH - 1) return NULL;
                    new_path[new_len++] = '/';
                }
                int tgt_len = strlen(child->inode->symlink_target);
                if (new_len + tgt_len >= VFS_MAX_PATH) return NULL;
                memcpy(new_path + new_len, child->inode->symlink_target, tgt_len);
                new_len += tgt_len;
            }

            /* Append remaining components */
            for (int j = i + 1; j < count; j++) {
                if (new_len >= VFS_MAX_PATH - 1) return NULL;
                new_path[new_len++] = '/';
                int cplen = strlen(components[j]);
                if (new_len + cplen >= VFS_MAX_PATH) return NULL;
                memcpy(new_path + new_len, components[j], cplen);
                new_len += cplen;
            }
            new_path[new_len] = '\0';

            /* Recursively resolve the new path with incremented depth */
            return vfs_resolve_dentry_internal(new_path, depth + 1, follow_last);
        }

        current = child;
    }

    /* Check if final dentry is a mount point */
    if (current->mounted && current->mount_root)
        return current->mount_root;

    return current;
}

/* Resolve a path to a dentry, following all symlinks */
static vfs_dentry_t *vfs_resolve_dentry(const char *path)
{
    return vfs_resolve_dentry_internal(path, 0, 1);
}

/* Resolve a path to a dentry, NOT following the final symlink */
static vfs_dentry_t *vfs_resolve_dentry_nofollow(const char *path)
{
    return vfs_resolve_dentry_internal(path, 0, 0);
}

vfs_inode_t *vfs_resolve_path(const char *path)
{
    vfs_dentry_t *d = vfs_resolve_dentry(path);
    return d ? d->inode : NULL;
}

/* ========================================================================
 * VFS initialization
 * ======================================================================== */

void vfs_init(void)
{
    /* Clear all state */
    for (int i = 0; i < VFS_MAX_FS_TYPES; i++)
        fs_types[i] = NULL;
    fs_type_count = 0;

    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mounts[i].active = 0;
    }
    mount_count = 0;

    for (int i = 0; i < VFS_MAX_FD; i++) {
        fd_table[i] = NULL;
        vfs_file_t **proc_fds = vfs_get_fd_table();
        if (proc_fds != fd_table)
            proc_fds[i] = NULL;
    }

    strcpy(cwd, "/");

    /* Create root inode */
    root_inode = (vfs_inode_t *)alloc_page();
    if (!root_inode) return;
    memset(root_inode, 0, PAGE_SIZE);
    root_inode->ino = 0;
    root_inode->type = VFS_TYPE_DIR;
    root_inode->mode = VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR |
                       VFS_S_IRGRP | VFS_S_IXGRP |
                       VFS_S_IROTH | VFS_S_IXOTH;
    root_inode->size = 0;
    root_inode->nlinks = 2;

    /* Create root dentry */
    root_dentry = dentry_alloc("/", root_inode);
    root_dentry->parent = root_dentry;  /* Root's parent is itself */

    printf("[VFS] Initialized, root at /\n");
}

/* ========================================================================
 * Filesystem registration
 * ======================================================================== */

int vfs_register_fs(vfs_filesystem_t *fs)
{
    if (fs_type_count >= VFS_MAX_FS_TYPES)
        return -1;

    fs_types[fs_type_count++] = fs;
    printf("[VFS] Registered filesystem: %s\n", fs->name);
    return 0;
}

/* ========================================================================
 * Mount/unmount
 * ======================================================================== */

int vfs_mount(const char *source, const char *target,
              const char *fstype, uint32_t flags, const char *options)
{
    /* Find filesystem type */
    vfs_filesystem_t *fs = NULL;
    for (int i = 0; i < fs_type_count; i++) {
        if (strcmp(fs_types[i]->name, fstype) == 0) {
            fs = fs_types[i];
            break;
        }
    }
    if (!fs) {
        printf("[VFS] Unknown filesystem type: %s\n", fstype);
        return -1;
    }

    /* Find a free mount slot */
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        printf("[VFS] Too many mounts\n");
        return -2;
    }

    /* Allocate superblock */
    vfs_superblock_t *sb = (vfs_superblock_t *)alloc_page();
    if (!sb) return -3;
    memset(sb, 0, PAGE_SIZE);
    sb->fs = fs;
    sb->blkdev_id = 0xFF;  /* No block device by default */

    /* Call filesystem mount */
    int ret = fs->mount(sb, 0xFF, options);
    if (ret != 0) {
        printf("[VFS] mount() failed for %s: %d\n", fstype, ret);
        return -4;
    }

    /* Record mount */
    mounts[slot].active = 1;
    strncpy(mounts[slot].path, target, VFS_MAX_PATH - 1);
    mounts[slot].sb = sb;

    /* Attach to dentry tree */
    if (strcmp(target, "/") == 0) {
        /* Mounting at root - replace root dentry */
        root_dentry = sb->root;
        root_inode = sb->root_inode;
        sb->root->parent = sb->root;
    } else {
        /* Mounting at a subdirectory - find the target dentry
         * and mark it as a mount point */
        vfs_dentry_t *mount_point = vfs_resolve_dentry(target);
        printf("[VFS_MOUNT] resolve '%s' => %p\n", target, (void *)mount_point);
        if (!mount_point) {
            /* Mount point doesn't exist - try to create it */
            vfs_mkdir(target, VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR |
                              VFS_S_IRGRP | VFS_S_IXGRP |
                              VFS_S_IROTH | VFS_S_IXOTH);
            mount_point = vfs_resolve_dentry(target);
            printf("[VFS_MOUNT] after mkdir, resolve '%s' => %p\n", target, (void *)mount_point);
        }
        if (mount_point) {
            mount_point->mounted = 1;
            mount_point->mount_root = sb->root;
            printf("[VFS_MOUNT] set mounted=1 on dentry '%s' at %p, mount_root=%p\n",
                   mount_point->name ? mount_point->name : "(null)", (void *)mount_point, (void *)sb->root);
            /* Verify: read back the flag */
            printf("[VFS_MOUNT] verify: dentry at %p, name='%s', mounted=%d, mount_root=%p\n",
                   (void *)mount_point, mount_point->name ? mount_point->name : "(null)",
                   mount_point->mounted, (void *)mount_point->mount_root);
        } else {
            printf("[VFS] Warning: mount point %s not found and could not be created\n", target);
        }
    }

    printf("[VFS] Mounted %s at %s\n", fstype, target);
    return 0;
}

int vfs_unmount(const char *target)
{
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].active && strcmp(mounts[i].path, target) == 0) {
            /* Call filesystem unmount */
            if (mounts[i].sb->fs->unmount) {
                mounts[i].sb->fs->unmount(mounts[i].sb);
            }
            mounts[i].active = 0;
            return 0;
        }
    }
    return -1;
}

/* ========================================================================
 * File descriptor management
 * ======================================================================== */

int vfs_alloc_fd(void)
{
    vfs_file_t **fds = vfs_get_fd_table();
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (fds[i] == NULL)
            return i;
    }
    return -1;
}

/* ========================================================================
 * Pipe operations
 * ======================================================================== */

int vfs_pipe(int fd[2])
{
    /* Allocate pipe data structure */
    vfs_pipe_t *pipe = (vfs_pipe_t *)alloc_page();
    if (!pipe) return -1;
    memset(pipe, 0, PAGE_SIZE);
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->count = 0;
    pipe->read_closed = 0;
    pipe->write_closed = 0;

    /* Allocate two file descriptors */
    int read_fd = vfs_alloc_fd();
    if (read_fd < 0) {
        free_page(pipe);
        return -2;
    }
    int write_fd = vfs_alloc_fd();
    if (write_fd < 0) {
        vfs_get_fd_table()[read_fd] = NULL;
        free_page(pipe);
        return -3;
    }

    /* Create read end inode */
    vfs_inode_t *read_inode = (vfs_inode_t *)alloc_page();
    if (!read_inode) {
        vfs_get_fd_table()[read_fd] = NULL;
        vfs_get_fd_table()[write_fd] = NULL;
        free_page(pipe);
        return -4;
    }
    memset(read_inode, 0, PAGE_SIZE);
    read_inode->type = VFS_TYPE_PIPE;

    /* Create write end inode */
    vfs_inode_t *write_inode = (vfs_inode_t *)alloc_page();
    if (!write_inode) {
        vfs_get_fd_table()[read_fd] = NULL;
        vfs_get_fd_table()[write_fd] = NULL;
        free_page(read_inode);
        free_page(pipe);
        return -5;
    }
    memset(write_inode, 0, PAGE_SIZE);
    write_inode->type = VFS_TYPE_PIPE;

    /* Create read end file */
    vfs_file_t *read_file = (vfs_file_t *)alloc_page();
    if (!read_file) {
        vfs_get_fd_table()[read_fd] = NULL;
        vfs_get_fd_table()[write_fd] = NULL;
        free_page(read_inode);
        free_page(write_inode);
        free_page(pipe);
        return -6;
    }
    memset(read_file, 0, PAGE_SIZE);
    read_file->inode = read_inode;
    read_file->flags = VFS_O_RDONLY;
    read_file->refcount = 1;
    read_file->pipe = pipe;

    /* Create write end file */
    vfs_file_t *write_file = (vfs_file_t *)alloc_page();
    if (!write_file) {
        vfs_get_fd_table()[read_fd] = NULL;
        vfs_get_fd_table()[write_fd] = NULL;
        free_page(read_file);
        free_page(read_inode);
        free_page(write_inode);
        free_page(pipe);
        return -7;
    }
    memset(write_file, 0, PAGE_SIZE);
    write_file->inode = write_inode;
    write_file->flags = VFS_O_WRONLY;
    write_file->refcount = 1;
    write_file->pipe = pipe;

    /* Store in fd table */
    pipe->read_fd = read_fd;
    pipe->write_fd = write_fd;
    vfs_get_fd_table()[read_fd] = read_file;
    vfs_get_fd_table()[write_fd] = write_file;

    fd[0] = read_fd;
    fd[1] = write_fd;
    return 0;
}

static int pipe_read(vfs_file_t *file, void *buf, size_t count)
{
    vfs_pipe_t *pipe = file->pipe;
    if (!pipe) return -1;

    if (pipe->count == 0) {
        /* If write end is closed, return EOF */
        if (pipe->write_closed)
            return 0;
        /* Write end still open but no data - return 0 for now (no blocking) */
        return 0;
    }

    /* Read min(count, available) bytes from circular buffer */
    size_t to_read = count;
    if (to_read > pipe->count)
        to_read = pipe->count;

    for (size_t i = 0; i < to_read; i++) {
        ((char *)buf)[i] = pipe->buf[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUF_SIZE;
    }
    pipe->count -= to_read;

    return (int)to_read;
}

static int pipe_write(vfs_file_t *file, const void *buf, size_t count)
{
    vfs_pipe_t *pipe = file->pipe;
    if (!pipe) return -1;

    if (pipe->read_closed) {
        /* Read end closed - can't write */
        return -1;
    }

    /* Write as much as fits */
    size_t available = PIPE_BUF_SIZE - pipe->count;
    size_t to_write = count;
    if (to_write > available)
        to_write = available;

    if (to_write == 0)
        return 0;

    for (size_t i = 0; i < to_write; i++) {
        pipe->buf[pipe->write_pos] = ((const char *)buf)[i];
        pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUF_SIZE;
    }
    pipe->count += to_write;

    return (int)to_write;
}

/* ========================================================================
 * File operations
 * ======================================================================== */

int vfs_open(const char *path, uint32_t flags, uint32_t mode)
{
    vfs_dentry_t *dentry = vfs_resolve_dentry(path);

    if (!dentry || !dentry->inode) {
        /* File doesn't exist - check O_CREAT */
        if (flags & VFS_O_CREAT) {
            /* For now, only memfs supports creation.
             * Find the parent directory and create the file there. */
            /* Find last component */
            const char *last_slash = NULL;
            for (const char *p = path; *p; p++) {
                if (*p == '/') last_slash = p;
            }

            char dir_path[VFS_MAX_PATH];
            char filename[VFS_MAX_NAME];

            if (last_slash) {
                int dir_len = last_slash - path + 1;
                if (dir_len >= VFS_MAX_PATH) return -1;
                memcpy(dir_path, path, dir_len);
                dir_path[dir_len] = '\0';
                strncpy(filename, last_slash + 1, VFS_MAX_NAME - 1);
            } else {
                strcpy(dir_path, cwd);
                strncpy(filename, path, VFS_MAX_NAME - 1);
            }

            vfs_dentry_t *parent = vfs_resolve_dentry(dir_path);
            if (!parent || !parent->inode || parent->inode->type != VFS_TYPE_DIR)
                return -2;

            /* Create new inode via filesystem's alloc_inode if available */
            vfs_inode_t *new_inode = NULL;
            if (parent->inode->sb && parent->inode->sb->fs &&
                parent->inode->sb->fs->alloc_inode) {
                new_inode = parent->inode->sb->fs->alloc_inode(parent->inode->sb);
            }
            if (!new_inode) {
                /* No alloc_inode available - cannot create a functional file */
                return -3;
            }
            new_inode->type = VFS_TYPE_FILE;
            new_inode->mode = mode;
            new_inode->size = 0;

            /* Call filesystem create callback if available */
            if (parent->inode->sb && parent->inode->sb->fs &&
                parent->inode->sb->fs->create) {
                int ret = parent->inode->sb->fs->create(parent, filename, new_inode);
                if (ret != 0) {
                    /* Create failed - clean up */
                    if (parent->inode->sb->fs->destroy_inode)
                        parent->inode->sb->fs->destroy_inode(new_inode);
                    return ret;
                }
            }

            /* Create dentry */
            vfs_dentry_t *new_dentry = dentry_alloc(filename, new_inode);
            if (!new_dentry) return -4;

            dentry_add_child(parent, new_dentry);
            dentry = new_dentry;
        } else {
            return -1;  /* File not found */
        }
    }

    /* Allocate file descriptor */
    int fd = vfs_alloc_fd();
    if (fd < 0) return -5;

    /* Create file object */
    vfs_file_t *file = (vfs_file_t *)alloc_page();
    if (!file) return -6;
    memset(file, 0, PAGE_SIZE);
    file->inode = dentry->inode;
    file->dentry = dentry;
    file->flags = flags;
    file->offset = 0;
    file->refcount = 1;

    /* If append mode, set offset to end */
    if (flags & VFS_O_APPEND)
        file->offset = dentry->inode->size;

    vfs_get_fd_table()[fd] = file;
    return fd;
}

int vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FD || !vfs_get_fd_table()[fd])
        return -1;

    vfs_file_t *file = vfs_get_fd_table()[fd];
    file->refcount--;

    /* Handle pipe close */
    if (file->inode && file->inode->type == VFS_TYPE_PIPE && file->pipe) {
        vfs_pipe_t *pipe = file->pipe;
        uint32_t accmode = file->flags & (VFS_O_RDONLY | VFS_O_WRONLY | VFS_O_RDWR);

        /* Only mark closed when the last reference is dropped */
        if (file->refcount <= 0) {
            if (accmode == VFS_O_RDONLY) {
                pipe->read_closed = 1;
            }
            if (accmode == VFS_O_WRONLY) {
                pipe->write_closed = 1;
            }

            /* Free this file's inode */
            free_page(file->inode);
            file->inode = NULL;

            /* When both ends are closed, free pipe data */
            if (pipe->read_closed && pipe->write_closed) {
                free_page(pipe);
            }
        }
    } else if (file->refcount <= 0) {
        /* Regular file - no longer referenced */
    }

    vfs_get_fd_table()[fd] = NULL;

    if (file->refcount <= 0) {
        free_page(file);
    }

    return 0;
}

int vfs_read(int fd, void *buf, size_t count)
{
    if (fd < 0 || fd >= VFS_MAX_FD || !vfs_get_fd_table()[fd])
        return -1;

    vfs_file_t *file = vfs_get_fd_table()[fd];
    if (!file->inode) return -2;

    /* Handle pipe read */
    if (file->inode->type == VFS_TYPE_PIPE && file->pipe) {
        return pipe_read(file, buf, count);
    }

    /* Use inode's read operation if available */
    if (file->inode->read)
        return file->inode->read(file, buf, count);

    /* Default read for memfs-like files */
    if (file->inode->type == VFS_TYPE_FILE && file->inode->fs_data) {
        uint64_t remaining = file->inode->size - file->offset;
        if ((size_t)remaining > count)
            remaining = count;
        if (remaining == 0)
            return 0;

        memcpy(buf, (uint8_t *)file->inode->fs_data + file->offset, remaining);
        file->offset += remaining;
        return (int)remaining;
    }

    return -3;
}

int vfs_write(int fd, const void *buf, size_t count)
{
    if (fd < 0 || fd >= VFS_MAX_FD || !vfs_get_fd_table()[fd])
        return -1;

    vfs_file_t *file = vfs_get_fd_table()[fd];
    if (!file->inode) return -2;

    /* Handle pipe write */
    if (file->inode->type == VFS_TYPE_PIPE && file->pipe) {
        return pipe_write(file, buf, count);
    }

    /* Use inode's write operation if available */
    if (file->inode->write)
        return file->inode->write(file, buf, count);

    /* Default write for memfs-like files */
    if (file->inode->type == VFS_TYPE_FILE && file->inode->fs_data) {
        uint64_t new_offset = file->offset + count;

        /* Check if we need more space (limited to one page for now) */
        if (new_offset > PAGE_SIZE)
            count = PAGE_SIZE - file->offset;

        if (count == 0)
            return 0;

        memcpy((uint8_t *)file->inode->fs_data + file->offset, buf, count);
        file->offset += count;

        if (file->offset > file->inode->size)
            file->inode->size = file->offset;

        return (int)count;
    }

    return -3;
}

int64_t vfs_seek(int fd, int64_t offset, int whence)
{
    if (fd < 0 || fd >= VFS_MAX_FD || !vfs_get_fd_table()[fd])
        return -1;

    vfs_file_t *file = vfs_get_fd_table()[fd];
    int64_t new_offset;

    switch (whence) {
    case VFS_SEEK_SET:
        new_offset = offset;
        break;
    case VFS_SEEK_CUR:
        new_offset = (int64_t)file->offset + offset;
        break;
    case VFS_SEEK_END:
        new_offset = (int64_t)file->inode->size + offset;
        break;
    default:
        return -2;
    }

    if (new_offset < 0)
        new_offset = 0;

    file->offset = (uint64_t)new_offset;
    return new_offset;
}

int vfs_fstat(int fd, vfs_inode_t *stat)
{
    if (fd < 0 || fd >= VFS_MAX_FD || !vfs_get_fd_table()[fd])
        return -1;

    vfs_file_t *file = vfs_get_fd_table()[fd];
    if (!file->inode) return -2;

    memcpy(stat, file->inode, sizeof(vfs_inode_t));
    return 0;
}

int vfs_mkdir(const char *path, uint32_t mode)
{
    /* Find parent directory */
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    char dir_path[VFS_MAX_PATH];
    char dirname[VFS_MAX_NAME];

    if (last_slash) {
        int dir_len = last_slash - path + 1;
        if (dir_len >= VFS_MAX_PATH) return -1;
        memcpy(dir_path, path, dir_len);
        dir_path[dir_len] = '\0';
        strncpy(dirname, last_slash + 1, VFS_MAX_NAME - 1);
    } else {
        strcpy(dir_path, cwd);
        strncpy(dirname, path, VFS_MAX_NAME - 1);
    }

    vfs_dentry_t *parent = vfs_resolve_dentry(dir_path);
    if (!parent || !parent->inode || parent->inode->type != VFS_TYPE_DIR)
        return -2;

    /* Check if already exists */
    if (dentry_lookup(parent, dirname))
        return -3;

    /* Create new inode via filesystem's alloc_inode if available */
    vfs_inode_t *new_inode = NULL;
    if (parent->inode->sb && parent->inode->sb->fs &&
        parent->inode->sb->fs->alloc_inode) {
        new_inode = parent->inode->sb->fs->alloc_inode(parent->inode->sb);
    }
    if (!new_inode) {
        /* No alloc_inode available - cannot create a functional directory */
        return -4;
    }
    new_inode->type = VFS_TYPE_DIR;
    new_inode->mode = mode | VFS_S_IXUSR;  /* Directories need execute bit */
    new_inode->size = 0;
    new_inode->nlinks = 2;

    /* Call filesystem mkdir callback if available */
    if (parent->inode->sb && parent->inode->sb->fs &&
        parent->inode->sb->fs->mkdir) {
        int ret = parent->inode->sb->fs->mkdir(parent, dirname, new_inode);
        if (ret != 0) {
            if (parent->inode->sb->fs->destroy_inode)
                parent->inode->sb->fs->destroy_inode(new_inode);
            return ret;
        }
    }
    /* Directories don't need data pages for memfs */
    if (new_inode->fs_data && !(parent->inode->sb && parent->inode->sb->fs &&
        parent->inode->sb->fs->mkdir)) {
        free_page(new_inode->fs_data);
        new_inode->fs_data = NULL;
    }

    /* Create dentry */
    vfs_dentry_t *new_dentry = dentry_alloc(dirname, new_inode);
    if (!new_dentry) return -5;

    dentry_add_child(parent, new_dentry);
    return 0;
}

int vfs_rmdir(const char *path)
{
    vfs_dentry_t *dentry = vfs_resolve_dentry(path);
    if (!dentry || !dentry->inode || dentry->inode->type != VFS_TYPE_DIR)
        return -1;

    /* Check if directory is empty */
    if (dentry->child)
        return -2;  /* Directory not empty */

    /* Call filesystem unlink callback if available (marks dir entry as deleted) */
    if (dentry->inode->sb && dentry->inode->sb->fs &&
        dentry->inode->sb->fs->unlink) {
        int ret = dentry->inode->sb->fs->unlink(dentry);
        if (ret != 0)
            return ret;
    }

    /* Remove from parent's child list */
    if (dentry->parent) {
        vfs_dentry_t **pp = &dentry->parent->child;
        while (*pp) {
            if (*pp == dentry) {
                *pp = dentry->next;
                break;
            }
            pp = &(*pp)->next;
        }
    }

    return 0;
}

int vfs_unlink(const char *path)
{
    vfs_dentry_t *dentry = vfs_resolve_dentry(path);
    if (!dentry || !dentry->inode || dentry->inode->type == VFS_TYPE_DIR)
        return -1;

    /* Call filesystem unlink callback if available */
    if (dentry->inode->sb && dentry->inode->sb->fs &&
        dentry->inode->sb->fs->unlink) {
        int ret = dentry->inode->sb->fs->unlink(dentry);
        if (ret != 0)
            return ret;
    }

    /* Remove from parent's child list */
    if (dentry->parent) {
        vfs_dentry_t **pp = &dentry->parent->child;
        while (*pp) {
            if (*pp == dentry) {
                *pp = dentry->next;
                break;
            }
            pp = &(*pp)->next;
        }
    }

    return 0;
}

int vfs_readdir(int fd, int index, char *name, int namelen, vfs_inode_t *stat)
{
    if (fd < 0 || fd >= VFS_MAX_FD || !vfs_get_fd_table()[fd])
        return -1;

    vfs_file_t *file = vfs_get_fd_table()[fd];
    if (!file->inode || file->inode->type != VFS_TYPE_DIR)
        return -2;

    /* Walk dentry children */
    vfs_dentry_t *dentry = file->dentry;
    if (!dentry) return -3;

    vfs_dentry_t *child = dentry->child;
    for (int i = 0; i < index && child; i++) {
        child = child->next;
    }

    if (!child || !child->inode)
        return 0;  /* No more entries */

    strncpy(name, child->name, namelen - 1);
    name[namelen - 1] = '\0';

    if (stat)
        memcpy(stat, child->inode, sizeof(vfs_inode_t));

    return 1;  /* Entry found */
}

const char *vfs_get_cwd(void)
{
    return cwd;
}

int vfs_chdir(const char *path)
{
    /* Verify the path exists and is a directory */
    vfs_dentry_t *dentry = vfs_resolve_dentry(path);
    if (!dentry || !dentry->inode || dentry->inode->type != VFS_TYPE_DIR)
        return -1;

    /* Update CWD */
    if (path[0] == '/') {
        strncpy(cwd, path, VFS_MAX_PATH - 1);
        cwd[VFS_MAX_PATH - 1] = '\0';
    } else {
        /* Simple relative path handling */
        int cwd_len = strlen(cwd);
        int path_len = strlen(path);
        if (cwd_len + 1 + path_len < VFS_MAX_PATH) {
            if (cwd[cwd_len - 1] != '/') {
                cwd[cwd_len] = '/';
                cwd_len++;
            }
            strcpy(cwd + cwd_len, path);
        }
    }

    /* Remove trailing slash (except for root) */
    int len = strlen(cwd);
    if (len > 1 && cwd[len - 1] == '/')
        cwd[len - 1] = '\0';

    return 0;
}

/* ========================================================================
 * File descriptor duplication
 * ======================================================================== */

int vfs_dup(int oldfd)
{
    if (oldfd < 0 || oldfd >= VFS_MAX_FD || !vfs_get_fd_table()[oldfd])
        return -1;

    int newfd = vfs_alloc_fd();
    if (newfd < 0)
        return -2;

    vfs_get_fd_table()[newfd] = vfs_get_fd_table()[oldfd];
    vfs_get_fd_table()[newfd]->refcount++;
    return newfd;
}

int vfs_dup2(int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= VFS_MAX_FD || !vfs_get_fd_table()[oldfd])
        return -1;
    if (newfd < 0 || newfd >= VFS_MAX_FD)
        return -2;
    if (oldfd == newfd)
        return newfd;

    /* Close newfd if it's already open */
    if (vfs_get_fd_table()[newfd])
        vfs_close(newfd);

    vfs_get_fd_table()[newfd] = vfs_get_fd_table()[oldfd];
    vfs_get_fd_table()[newfd]->refcount++;
    return newfd;
}

/* ========================================================================
 * Symlink operations
 * ======================================================================== */

int vfs_symlink(const char *target, const char *linkpath)
{
    /* Find parent directory */
    const char *last_slash = NULL;
    for (const char *p = linkpath; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    char dir_path[VFS_MAX_PATH];
    char linkname[VFS_MAX_NAME];

    if (last_slash) {
        int dir_len = last_slash - linkpath + 1;
        if (dir_len >= VFS_MAX_PATH) return -1;
        memcpy(dir_path, linkpath, dir_len);
        dir_path[dir_len] = '\0';
        strncpy(linkname, last_slash + 1, VFS_MAX_NAME - 1);
        linkname[VFS_MAX_NAME - 1] = '\0';
    } else {
        strcpy(dir_path, cwd);
        strncpy(linkname, linkpath, VFS_MAX_NAME - 1);
        linkname[VFS_MAX_NAME - 1] = '\0';
    }

    vfs_dentry_t *parent = vfs_resolve_dentry(dir_path);
    if (!parent || !parent->inode || parent->inode->type != VFS_TYPE_DIR)
        return -2;

    /* Check if already exists */
    if (dentry_lookup(parent, linkname))
        return -3;

    /* Create new inode */
    vfs_inode_t *new_inode = NULL;
    if (parent->inode->sb && parent->inode->sb->fs &&
        parent->inode->sb->fs->alloc_inode) {
        new_inode = parent->inode->sb->fs->alloc_inode(parent->inode->sb);
    }
    if (!new_inode) {
        new_inode = (vfs_inode_t *)alloc_page();
        if (!new_inode) return -4;
        memset(new_inode, 0, PAGE_SIZE);
        new_inode->ino = (uint64_t)(uintptr_t)new_inode;
        new_inode->sb = parent->inode->sb;
    }
    new_inode->type = VFS_TYPE_SYMLINK;
    new_inode->mode = VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR |
                      VFS_S_IRGRP | VFS_S_IXGRP |
                      VFS_S_IROTH | VFS_S_IXOTH;
    new_inode->size = strlen(target);
    strncpy(new_inode->symlink_target, target, VFS_MAX_PATH - 1);
    new_inode->symlink_target[VFS_MAX_PATH - 1] = '\0';

    /* Create dentry */
    vfs_dentry_t *new_dentry = dentry_alloc(linkname, new_inode);
    if (!new_dentry) {
        if (parent->inode->sb && parent->inode->sb->fs &&
            parent->inode->sb->fs->destroy_inode)
            parent->inode->sb->fs->destroy_inode(new_inode);
        else
            free_page(new_inode);
        return -5;
    }

    dentry_add_child(parent, new_dentry);
    return 0;
}

int vfs_readlink(const char *path, char *buf, size_t bufsiz)
{
    /* Resolve without following the final symlink */
    vfs_dentry_t *dentry = vfs_resolve_dentry_nofollow(path);
    if (!dentry || !dentry->inode)
        return -1;

    if (dentry->inode->type != VFS_TYPE_SYMLINK)
        return -2;  /* Not a symlink */

    size_t len = strlen(dentry->inode->symlink_target);
    if (len > bufsiz)
        len = bufsiz;

    memcpy(buf, dentry->inode->symlink_target, len);
    return (int)len;
}

/* ========================================================================
 * VFS Self-Test
 * ======================================================================== */

int vfs_selftest(void)
{
    int passed = 0, failed = 0;

    /* Test 1: ls / */
    {
        int fd = vfs_open("/", VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
        if (fd >= 0) {
            char name[VFS_MAX_NAME]; vfs_inode_t stat;
            int cnt = 0, idx = 0;
            while (vfs_readdir(fd, idx, name, VFS_MAX_NAME, &stat) > 0) { cnt++; idx++; }
            vfs_close(fd);
            printf("[1] ls / : OK (%d entries)\n", cnt);
            passed++;
        } else { printf("[1] ls / : FAILED (fd=%d)\n", fd); failed++; }
    }

    /* Test 2: mkdir /tmp */
    {
        int ret = vfs_mkdir("/tmp", VFS_S_IRUSR|VFS_S_IWUSR|VFS_S_IXUSR);
        if (ret == 0) { printf("[2] mkdir /tmp : OK\n"); passed++; }
        else { printf("[2] mkdir /tmp : FAILED (%d)\n", ret); failed++; }
    }

    /* Test 3: create /hello.txt */
    {
        int fd = vfs_open("/hello.txt", VFS_O_CREAT|VFS_O_WRONLY, VFS_S_IRUSR|VFS_S_IWUSR);
        if (fd >= 0) { vfs_close(fd); printf("[3] touch /hello.txt : OK\n"); passed++; }
        else { printf("[3] touch /hello.txt : FAILED (%d)\n", fd); failed++; }
    }

    /* Test 4: write /hello.txt */
    {
        int fd = vfs_open("/hello.txt", VFS_O_WRONLY, 0);
        if (fd >= 0) {
            const char *msg = "Hello, SpiritFoxOS VFS!";
            int n = vfs_write(fd, msg, strlen(msg));
            vfs_close(fd);
            if (n == (int)strlen(msg)) { printf("[4] write /hello.txt : OK (%d bytes)\n", n); passed++; }
            else { printf("[4] write /hello.txt : PARTIAL (%d/%d)\n", n, (int)strlen(msg)); failed++; }
        } else { printf("[4] write /hello.txt : FAILED (%d)\n", fd); failed++; }
    }

    /* Test 5: read /hello.txt */
    {
        int fd = vfs_open("/hello.txt", VFS_O_RDONLY, 0);
        if (fd >= 0) {
            char buf[128];
            int n = vfs_read(fd, buf, sizeof(buf)-1);
            vfs_close(fd);
            if (n > 0) { buf[n] = '\0'; printf("[5] read /hello.txt : OK (\"%s\")\n", buf); passed++; }
            else { printf("[5] read /hello.txt : FAILED (n=%d)\n", n); failed++; }
        } else { printf("[5] read /hello.txt : FAILED (%d)\n", fd); failed++; }
    }

    /* Test 6: ls /dev (devfs) */
    {
        int fd = vfs_open("/dev", VFS_O_RDONLY|VFS_O_DIRECTORY, 0);
        if (fd >= 0) {
            char name[VFS_MAX_NAME]; vfs_inode_t stat;
            int cnt = 0, idx = 0;
            while (vfs_readdir(fd, idx, name, VFS_MAX_NAME, &stat) > 0) { cnt++; idx++; }
            vfs_close(fd);
            printf("[6] ls /dev : OK (%d entries)\n", cnt);
            passed++;
        } else { printf("[6] ls /dev : FAILED (%d)\n", fd); failed++; }
    }

    /* Test 7: cd /tmp + pwd */
    {
        int ret = vfs_chdir("/tmp");
        if (ret == 0 && strcmp(vfs_get_cwd(), "/tmp") == 0) {
            printf("[7] cd /tmp : OK (cwd=%s)\n", vfs_get_cwd());
            passed++;
        } else { printf("[7] cd /tmp : FAILED (cwd=%s)\n", vfs_get_cwd()); failed++; }
        vfs_chdir("/");
    }

    /* Test 8: rm /hello.txt + verify */
    {
        int ret = vfs_unlink("/hello.txt");
        if (ret == 0) {
            int fd2 = vfs_open("/hello.txt", VFS_O_RDONLY, 0);
            if (fd2 < 0) { printf("[8] rm + verify : OK\n"); passed++; }
            else { vfs_close(fd2); printf("[8] rm + verify : FAILED (still exists)\n"); failed++; }
        } else { printf("[8] rm : FAILED (%d)\n", ret); failed++; }
    }

    /* Test 9: seek test */
    {
        int fd = vfs_open("/seektest", VFS_O_CREAT|VFS_O_WRONLY, VFS_S_IRUSR|VFS_S_IWUSR);
        if (fd >= 0) {
            vfs_write(fd, "ABCDEFGHIJ", 10);
            vfs_close(fd);
        }
        fd = vfs_open("/seektest", VFS_O_RDONLY, 0);
        if (fd >= 0) {
            vfs_seek(fd, 5, VFS_SEEK_SET);
            char buf[8];
            int n = vfs_read(fd, buf, 3);
            vfs_close(fd);
            if (n == 3 && buf[0] == 'F' && buf[1] == 'G' && buf[2] == 'H') {
                printf("[9] seek test : OK\n");
                passed++;
            } else {
                printf("[9] seek test : FAILED (n=%d, buf=%c%c%c)\n", n, buf[0], buf[1], buf[2]);
                failed++;
            }
        } else { printf("[9] seek test : FAILED (open)\n"); failed++; }
        vfs_unlink("/seektest");
    }

    printf("VFS Self-Test: %d/%d passed\n", passed, passed+failed);

    /* Test 10: Pipe test */
    {
        int pipefd[2];
        int ret = vfs_pipe(pipefd);
        if (ret == 0) {
            vfs_write(pipefd[1], "pipe test", 9);
            char buf[32];
            int n = vfs_read(pipefd[0], buf, 9);
            if (n == 9) {
                buf[n] = '\0';
                if (memcmp(buf, "pipe test", 9) == 0) {
                    printf("[10] pipe test : OK (\"%s\")\n", buf);
                    passed++;
                } else {
                    printf("[10] pipe test : FAILED (data mismatch)\n");
                    failed++;
                }
            } else {
                printf("[10] pipe test : FAILED (read %d bytes)\n", n);
                failed++;
            }
            vfs_close(pipefd[0]);
            vfs_close(pipefd[1]);
        } else {
            printf("[10] pipe test : FAILED (vfs_pipe returned %d)\n", ret);
            failed++;
        }
    }

    printf("VFS Self-Test (with pipe): %d/%d passed\n", passed, passed+failed);
    return failed;
}
