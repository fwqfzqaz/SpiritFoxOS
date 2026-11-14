#ifndef SF_LINUX_ABI_H
#define SF_LINUX_ABI_H
/* Linux x86_64 用户态 ABI 子集（S3）。
 *
 * 本头文件只包含常量与结构布局，不依赖 SpiritFoxOS 运行时；翻译器、静态 PIE
 * 夹具与宿主测试共用同一份定义，避免结构尺寸漂移。 */
#include <stdint.h>
#include <stddef.h>

/* ---- 系统调用号（x86_64） ---- */
#define LX_SYS_READ 0u
#define LX_SYS_WRITE 1u
#define LX_SYS_OPEN 2u
#define LX_SYS_CLOSE 3u
#define LX_SYS_FSTAT 5u
#define LX_SYS_POLL 7u
#define LX_SYS_LSEEK 8u
#define LX_SYS_MMAP 9u
#define LX_SYS_MPROTECT 10u
#define LX_SYS_MUNMAP 11u
#define LX_SYS_BRK 12u
#define LX_SYS_IOCTL 16u
#define LX_SYS_PREAD64 17u
#define LX_SYS_SCHED_YIELD 24u
#define LX_SYS_DUP 32u
#define LX_SYS_DUP2 33u
#define LX_SYS_NANOSLEEP 35u
#define LX_SYS_GETPID 39u
#define LX_SYS_EXIT 60u
#define LX_SYS_UNAME 63u
#define LX_SYS_FCNTL 72u
#define LX_SYS_GETCWD 79u
#define LX_SYS_GETTIMEOFDAY 96u
#define LX_SYS_SYSINFO 99u
#define LX_SYS_PTRACE 101u
#define LX_SYS_GETUID 102u
#define LX_SYS_SETUID 105u
#define LX_SYS_SETGID 106u
#define LX_SYS_SETRESUID 117u
#define LX_SYS_SETRESGID 119u
#define LX_SYS_ARCH_PRCTL 158u
#define LX_SYS_MOUNT 165u
#define LX_SYS_UMOUNT2 166u
#define LX_SYS_SWAPON 167u
#define LX_SYS_SWAPOFF 168u
#define LX_SYS_REBOOT 169u
#define LX_SYS_INIT_MODULE 175u
#define LX_SYS_DELETE_MODULE 176u
#define LX_SYS_GETTID 186u
#define LX_SYS_GETDENTS64 217u
#define LX_SYS_SET_TID_ADDRESS 218u
#define LX_SYS_CLOCK_GETTIME 228u
#define LX_SYS_EXIT_GROUP 231u
#define LX_SYS_KEXEC_LOAD 246u
#define LX_SYS_OPENAT 257u
#define LX_SYS_NEWFSTATAT 262u
#define LX_SYS_READLINKAT 267u
#define LX_SYS_PPOLL 271u
#define LX_SYS_DUP3 292u
#define LX_SYS_PIPE2 293u
#define LX_SYS_PRLIMIT64 302u
#define LX_SYS_PROCESS_VM_READV 310u
#define LX_SYS_PROCESS_VM_WRITEV 311u
#define LX_SYS_FINIT_MODULE 313u
#define LX_SYS_GETRANDOM 318u
#define LX_SYS_BPF 321u
#define LX_SYS_USERFAULTFD 323u
#define LX_SYS_IO_URING_SETUP 425u
#define LX_SYS_IO_URING_ENTER 426u
#define LX_SYS_IO_URING_REGISTER 427u

/* ---- errno ---- */
#define LX_EPERM 1
#define LX_ENOENT 2
#define LX_ESRCH 3
#define LX_EINTR 4
#define LX_EIO 5
#define LX_EBADF 9
#define LX_EAGAIN 11
#define LX_ENOMEM 12
#define LX_EACCES 13
#define LX_EFAULT 14
#define LX_EBUSY 16
#define LX_ENODEV 19
#define LX_ENOTDIR 20
#define LX_EISDIR 21
#define LX_EINVAL 22
#define LX_EMFILE 24
#define LX_ENOTTY 25
#define LX_ENOSPC 28
#define LX_ESPIPE 29
#define LX_EROFS 30
#define LX_EPIPE 32
#define LX_ERANGE 34
#define LX_ENOSYS 38
#define LX_ENOTSUP 95
#define LX_ETIMEDOUT 110

/* ---- open/fcntl/stat ---- */
#define LX_O_ACCMODE 3
#define LX_O_RDONLY 0
#define LX_O_WRONLY 1
#define LX_O_RDWR 2
#define LX_O_CREAT 0x40
#define LX_O_EXCL 0x80
#define LX_O_TRUNC 0x200
#define LX_O_APPEND 0x400
#define LX_O_NONBLOCK 0x800
#define LX_O_DIRECTORY 0x10000
#define LX_O_NOFOLLOW 0x20000
#define LX_O_CLOEXEC 0x80000

#define LX_AT_FDCWD (-100)
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_EMPTY_PATH 0x1000

#define LX_SEEK_SET 0
#define LX_SEEK_CUR 1
#define LX_SEEK_END 2

#define LX_F_DUPFD 0
#define LX_F_GETFD 1
#define LX_F_SETFD 2
#define LX_F_GETFL 3
#define LX_F_SETFL 4
#define LX_F_DUPFD_CLOEXEC 1030

#define LX_S_IFMT 0170000
#define LX_S_IFIFO 0010000
#define LX_S_IFCHR 0020000
#define LX_S_IFDIR 0040000
#define LX_S_IFREG 0100000

#define LX_DT_UNKNOWN 0
#define LX_DT_FIFO 1
#define LX_DT_CHR 2
#define LX_DT_DIR 4
#define LX_DT_REG 8

/* ---- mmap/mprotect/poll/ioctl ---- */
#define LX_PROT_NONE 0
#define LX_PROT_READ 1
#define LX_PROT_WRITE 2
#define LX_PROT_EXEC 4
#define LX_MAP_SHARED 1
#define LX_MAP_PRIVATE 2
#define LX_MAP_FIXED 0x10
#define LX_MAP_ANONYMOUS 0x20

#define LX_POLLIN 0x001
#define LX_POLLOUT 0x004
#define LX_POLLERR 0x008
#define LX_POLLHUP 0x010
#define LX_POLLNVAL 0x020

#define LX_TIOCGWINSZ 0x5413
#define LX_ARCH_SET_FS 0x1002
#define LX_ARCH_GET_FS 0x1003

#define LX_RLIMIT_CPU 0
#define LX_RLIMIT_FSIZE 1
#define LX_RLIMIT_DATA 2
#define LX_RLIMIT_STACK 3
#define LX_RLIMIT_CORE 4
#define LX_RLIMIT_RSS 5
#define LX_RLIMIT_NPROC 6
#define LX_RLIMIT_NOFILE 7
#define LX_RLIMIT_MEMLOCK 8
#define LX_RLIMIT_AS 9
#define LX_RLIMIT_LOCKS 10
#define LX_RLIMIT_SIGPENDING 11
#define LX_RLIMIT_MSGQUEUE 12
#define LX_RLIMIT_NICE 13
#define LX_RLIMIT_RTPRIO 14
#define LX_RLIMIT_RTTIME 15

/* ---- 结构布局 ---- */
typedef struct { uint64_t tv_sec,tv_nsec; } LxTimespec;
typedef struct { uint64_t tv_sec,tv_usec; } LxTimeval;
typedef struct { int32_t tv_sec,tv_usec; } LxTimeval32;
typedef struct { int32_t fd; int16_t events,revents; } LxPollfd;
_Static_assert(sizeof(LxPollfd)==8,"pollfd layout");
typedef struct { uint16_t ws_row,ws_col,ws_xpixel,ws_ypixel; } LxWinsize;
_Static_assert(sizeof(LxWinsize)==8,"winsize layout");
typedef struct { uint64_t cur,max; } LxRlimit;
_Static_assert(sizeof(LxRlimit)==16,"rlimit layout");

/* x86_64 struct stat：144 字节，字段顺序与 Linux 内核 UAPI 一致。 */
typedef struct {
 uint64_t st_dev;
 uint64_t st_ino;
 uint64_t st_nlink;
 uint32_t st_mode;
 uint32_t st_uid;
 uint32_t st_gid;
 uint32_t __pad0;
 uint64_t st_rdev;
 uint64_t st_size;
 uint64_t st_blksize;
 uint64_t st_blocks;
 uint64_t st_atime;
 uint64_t st_atime_nsec;
 uint64_t st_mtime;
 uint64_t st_mtime_nsec;
 uint64_t st_ctime;
 uint64_t st_ctime_nsec;
 uint64_t __reserved[3];
} LxStat;
_Static_assert(sizeof(LxStat)==144,"stat layout");

typedef struct { char sysname[65],nodename[65],release[65],version[65],machine[65],domainname[65]; } LxUtsname;
_Static_assert(sizeof(LxUtsname)==390,"utsname layout");

/* dirent64 的 d_name 位于偏移 19，记录按 8 字节对齐。 */
typedef struct {
 uint64_t d_ino;
 int64_t d_off;
 uint16_t d_reclen;
 uint8_t d_type;
 char d_name[];
} LxDirent64;
#define LX_DIRENT_NAME_OFF 19u

/* struct sysinfo：112 字节。 */
typedef struct {
 int64_t uptime;
 uint64_t loads[3];
 uint64_t totalram,freeram,sharedram,bufferram,totalswap,freeswap;
 uint16_t procs;
 uint16_t __pad;
 uint64_t totalhigh,freehigh;
 uint32_t mem_unit;
 char __reserved[4];
} LxSysinfo;
_Static_assert(sizeof(LxSysinfo)==112,"sysinfo layout");

/* 内核写入用户帧页的完整寄存器集合（与 kernel Frame 顺序一致）。 */
typedef struct {
 uint64_t r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rbx,rdx,rcx,rax;
 uint64_t vector,error,rip,cs,rflags,rsp,ss;
} LxFrame;
_Static_assert(offsetof(LxFrame,rax)==112,"frame rax offset");
_Static_assert(offsetof(LxFrame,rip)==136,"frame rip offset");
_Static_assert(offsetof(LxFrame,ss)==168,"frame ss offset");

#endif
