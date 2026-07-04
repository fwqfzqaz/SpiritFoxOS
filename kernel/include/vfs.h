#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

/* ========================================================================
 * VFS type definitions
 * ======================================================================== */

/* File types */
#define VFS_TYPE_NONE      0
#define VFS_TYPE_FILE      1
#define VFS_TYPE_DIR       2
#define VFS_TYPE_CHARDEV   3
#define VFS_TYPE_BLKDEV    4
#define VFS_TYPE_SYMLINK   5
#define VFS_TYPE_FIFO      6
#define VFS_TYPE_PIPE      7

/* Open flags */
#define VFS_O_RDONLY    0x00
#define VFS_O_WRONLY    0x01
#define VFS_O_RDWR      0x02
#define VFS_O_CREAT     0x40
#define VFS_O_TRUNC     0x80
#define VFS_O_APPEND    0x100
#define VFS_O_DIRECTORY 0x200

/* Seek whence */
#define VFS_SEEK_SET    0
#define VFS_SEEK_CUR    1
#define VFS_SEEK_END    2

/* Permission bits */
#define VFS_S_IXOTH    0x001
#define VFS_S_IWOTH    0x002
#define VFS_S_IROTH    0x004
#define VFS_S_IXGRP    0x008
#define VFS_S_IWGRP    0x010
#define VFS_S_IRGRP    0x020
#define VFS_S_IXUSR    0x040
#define VFS_S_IWUSR    0x080
#define VFS_S_IRUSR    0x100

/* Limits */
#define VFS_MAX_PATH     256
#define VFS_MAX_NAME     64
#define VFS_MAX_FD       32
#define VFS_MAX_MOUNTS   8

/* Pipe buffer size (must be power of 2 for efficient wrapping) */
#define PIPE_BUF_SIZE  4096

/* Pipe data structure */
typedef struct {
    char     buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;       /* Bytes currently in buffer */
    int      read_fd;     /* Read end fd */
    int      write_fd;    /* Write end fd */
    int      read_closed; /* Read end closed flag */
    int      write_closed;/* Write end closed flag */
} vfs_pipe_t;

/* Forward declarations */
typedef struct vfs_inode vfs_inode_t;
typedef struct vfs_dentry vfs_dentry_t;
typedef struct vfs_file vfs_file_t;
typedef struct vfs_superblock vfs_superblock_t;
typedef struct vfs_filesystem vfs_filesystem_t;

/* ========================================================================
 * Inode - describes a file's metadata
 * ======================================================================== */

struct vfs_inode {
    uint64_t        ino;           /* Inode number */
    uint32_t        type;          /* File type (VFS_TYPE_*) */
    uint32_t        mode;          /* Permission bits */
    uint64_t        size;          /* File size in bytes */
    uint64_t        nlinks;        /* Hard link count */
    uint32_t        uid;           /* Owner user ID */
    uint32_t        gid;           /* Owner group ID */
    uint64_t        atime;         /* Access time */
    uint64_t        mtime;         /* Modification time */
    uint64_t        ctime;         /* Change time */
    uint32_t        blkdev_id;     /* Block device ID (for VFS_TYPE_BLKDEV) */
    uint32_t        chardev_id;    /* Char device ID (for VFS_TYPE_CHARDEV) */

    /* Symlink target path (for VFS_TYPE_SYMLINK) */
    char            symlink_target[VFS_MAX_PATH];

    /* Filesystem-specific data */
    void           *fs_data;

    /* Reference to superblock */
    vfs_superblock_t *sb;

    /* Operations */
    int (*read)(vfs_file_t *file, void *buf, size_t count);
    int (*write)(vfs_file_t *file, const void *buf, size_t count);
    int (*truncate)(vfs_inode_t *inode, uint64_t size);
};

/* ========================================================================
 * Directory entry - maps name to inode
 * ======================================================================== */

struct vfs_dentry {
    char            name[VFS_MAX_NAME];  /* Entry name */
    vfs_inode_t    *inode;               /* Associated inode */
    vfs_dentry_t   *parent;              /* Parent dentry */
    vfs_dentry_t   *child;               /* First child (for directories) */
    vfs_dentry_t   *next;                /* Next sibling */
    int             mounted;             /* Is this a mount point? */
    vfs_dentry_t   *mount_root;          /* Root dentry of mounted fs */
};

/* ========================================================================
 * Open file - represents an open file descriptor
 * ======================================================================== */

struct vfs_file {
    vfs_inode_t    *inode;         /* Associated inode */
    uint64_t        offset;        /* Current file offset */
    uint32_t        flags;         /* Open flags (VFS_O_*) */
    uint32_t        refcount;      /* Reference count */
    vfs_dentry_t   *dentry;        /* Associated dentry */
    vfs_pipe_t     *pipe;          /* Pipe data (for VFS_TYPE_PIPE) */
};

/* ========================================================================
 * Superblock - represents a mounted filesystem instance
 * ======================================================================== */

struct vfs_superblock {
    vfs_filesystem_t *fs;          /* Filesystem type */
    vfs_dentry_t     *root;        /* Root dentry of this mount */
    vfs_inode_t      *root_inode;  /* Root inode */
    void             *fs_data;     /* Filesystem-specific data */
    uint8_t           blkdev_id;   /* Block device (0xFF = none) */
};

/* ========================================================================
 * Filesystem type - describes a filesystem implementation
 * ======================================================================== */

struct vfs_filesystem {
    char    name[16];             /* Filesystem name (e.g., "memfs", "fat32") */
    int (*mount)(vfs_superblock_t *sb, uint8_t blkdev_id, const char *options);
    int (*unmount)(vfs_superblock_t *sb);
    vfs_inode_t *(*alloc_inode)(vfs_superblock_t *sb);
    void (*destroy_inode)(vfs_inode_t *inode);
    int (*create)(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode);
    int (*mkdir)(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode);
    int (*unlink)(vfs_dentry_t *dentry);
};

/* ========================================================================
 * Global VFS state
 * ======================================================================== */

/* Mount point descriptor */
typedef struct {
    int             active;
    char            path[VFS_MAX_PATH];  /* Mount path */
    vfs_superblock_t *sb;                /* Superblock */
} vfs_mount_t;

/* ========================================================================
 * VFS API
 * ======================================================================== */

/* Initialize VFS subsystem */
void vfs_init(void);

/* Register a filesystem type */
int vfs_register_fs(vfs_filesystem_t *fs);

/* Mount a filesystem at the given path */
int vfs_mount(const char *source, const char *target,
              const char *fstype, uint32_t flags, const char *options);

/* Unmount a filesystem */
int vfs_unmount(const char *target);

/* Open a file */
int vfs_open(const char *path, uint32_t flags, uint32_t mode);

/* Close a file descriptor */
int vfs_close(int fd);

/* Read from a file descriptor */
int vfs_read(int fd, void *buf, size_t count);

/* Write to a file descriptor */
int vfs_write(int fd, const void *buf, size_t count);

/* Seek in a file descriptor */
int64_t vfs_seek(int fd, int64_t offset, int whence);

/* Get file status */
int vfs_fstat(int fd, vfs_inode_t *stat);

/* Create a directory */
int vfs_mkdir(const char *path, uint32_t mode);

/* Remove a directory */
int vfs_rmdir(const char *path);

/* Unlink (delete) a file */
int vfs_unlink(const char *path);

/* List directory contents */
int vfs_readdir(int fd, int index, char *name, int namelen, vfs_inode_t *stat);

/* Resolve a path to an inode */
vfs_inode_t *vfs_resolve_path(const char *path);

/* Get current working directory */
const char *vfs_get_cwd(void);

/* Change current working directory */
int vfs_chdir(const char *path);

/* Get the file descriptor table for the current process (simple single-process) */
vfs_file_t **vfs_get_fd_table(void);

/* Allocate a file descriptor */
int vfs_alloc_fd(void);

/* Create a pipe, returns two file descriptors: fd[0] for reading, fd[1] for writing */
int vfs_pipe(int fd[2]);

/* Duplicate file descriptor */
int vfs_dup(int oldfd);

/* Duplicate file descriptor to a specific new fd */
int vfs_dup2(int oldfd, int newfd);

/* Create a symbolic link */
int vfs_symlink(const char *target, const char *linkpath);

/* Read the target of a symbolic link */
int vfs_readlink(const char *path, char *buf, size_t bufsiz);

#endif /* VFS_H */
