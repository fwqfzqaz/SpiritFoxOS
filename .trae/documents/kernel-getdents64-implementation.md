# getdents64 内核实现计划

## Context

libc Phase 1 已完成，但 `sys_getdents64` 当前是桩函数（返回 0），导致用户态 `readdir()` 始终返回 NULL，`ls` 等工具无法工作。内核已有 `vfs_readdir(fd, index, name, namelen, stat)` 函数可以逐条读取目录项，需要将其与 Linux `getdents64` 系统调用格式对接。

## 核心思路

`vfs_readdir()` 使用 index 遍历 dentry 的 child 链表，而 `getdents64` 需要将目录项填充到 `linux_dirent64` 结构体数组中返回。实现方案：

1. 在 `vfs_file_t` 中增加 `dir_index` 字段，记录目录读取位置（类似 Linux 的 `f_pos`）
2. 在内核 `syscall.h` 中定义 `linux_dirent64` 结构体
3. 实现 `sys_getdents64`，循环调用 `vfs_readdir` 并填充 `linux_dirent64` 条目
4. VFS 类型到 d_type 的映射

## 关键文件

### 1. `kernel/include/syscall.h` — 新增 linux_dirent64 定义

```c
/* Linux-compatible dirent64 structure */
typedef struct {
    uint64_t d_ino;           /* Inode number */
    int64_t  d_off;           /* Offset to next dirent */
    uint16_t d_reclen;        /* Length of this dirent */
    uint8_t  d_type;          /* Type of file */
    char     d_name[];        /* Filename (null-terminated) */
} linux_dirent64_t;
```

`d_reclen` = `offsetof(linux_dirent64_t, d_name) + strlen(name) + 1`，然后向上对齐到 8 字节。

### 2. `kernel/include/vfs.h` — vfs_file_t 增加 dir_index

在 `vfs_file` 结构体中添加：
```c
uint32_t dir_index;  /* 目录读取位置（用于 getdents64） */
```

### 3. `kernel/src/syscall/syscall_file.c` — 实现 sys_getdents64

核心逻辑：
- 从 frame 提取参数：fd、dirp（用户态缓冲区）、count（缓冲区大小）
- 通过 fd 获取 vfs_file_t，验证是目录
- 用 `file->dir_index` 作为当前读取位置
- 循环：调用 `vfs_readdir(fd, dir_index, name, ...)` 逐条读取
- 对每个条目：填充 `linux_dirent64_t`，将 VFS 类型映射到 d_type
- 将填充好的条目拷贝到用户态缓冲区
- 更新 `file->dir_index`
- 返回总写入字节数

VFS_TYPE → d_type 映射：
| VFS_TYPE | d_type |
|----------|--------|
| VFS_TYPE_FILE | DT_REG (8) |
| VFS_TYPE_DIR | DT_DIR (4) |
| VFS_TYPE_CHARDEV | DT_CHR (2) |
| VFS_TYPE_BLKDEV | DT_BLK (6) |
| VFS_TYPE_SYMLINK | DT_LNK (10) |
| VFS_TYPE_FIFO | DT_FIFO (1) |
| VFS_TYPE_PIPE | DT_FIFO (1) |
| default | DT_UNKNOWN (0) |

### 4. `kernel/src/fs/vfs.c` — vfs_open 初始化 dir_index

在 `vfs_open` 创建 file 对象时，将 `file->dir_index = 0`。已有 `memset(file, 0, ...)` 所以无需额外操作。

同时需要修改 `vfs_readdir`：它当前通过 `index` 参数遍历 child 链表，但由于链表可能被修改（增删目录项），使用 index 遍历在并发环境下不安全。不过对于单线程内核这是可接受的。当前实现是正确的。

## 验证

1. 编译内核确认无错误
2. 在 QEMU 中运行，通过 `ls /` 命令验证目录列表输出
3. 验证 libc 侧 `opendir/readdir/closedir` 能正确获取目录项
