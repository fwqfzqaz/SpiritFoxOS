#ifndef SF_H
#define SF_H
#include <stdint.h>
#include <stddef.h>
#define SF_ABI 1
#define SF_PAGE 4096ull
#define SF_BASE 0x40000000ull
#define SF_IMAGE_MAX 0x800000ull
#define SF_PIE_MIN 0x44000000ull
#define SF_NATIVE_STATIC 0u
#define SF_NATIVE_RELATIVE 1u
#define SF_RELOCATION_MAX 256u
#define SF_STACK 0x41010000ull
#define SF_ARGS 0x42000000ull
#define SF_BUNDLE 0x60000000ull
#define SF_DMA 0x50000000ull
#define SF_DMA_BYTES 65536u
/* 用户侧 framebuffer 映射基点：仅经 SYS_FB_MAP 授权后可见。 */
#define SF_FB 0x70000000ull
#define SF_FB_BYTES 0x4000000ull
/* 匿名堆窗口：SYS_MMAP 只在该区间内建立映射（每进程独立页表）。 */
#define SF_HEAP 0x74000000ull
#define SF_HEAP_BYTES 0x4000000ull
/* 共享内存窗口：固定槽位映射，8 槽 × 8 MiB。 */
#define SF_SHM 0x78000000ull
#define SF_SHM_BYTES 0x4000000ull
#define SF_SHM_SLOTS 8u
#define SF_SHM_SLOT_BYTES 0x800000ull
/* Relocatable images occupy a separate, 2 MiB aligned arena below DMA. */
static inline int sf_image_base_valid(uint64_t base){return !base||base==SF_BASE||(base>=SF_PIE_MIN&&base<=SF_DMA-SF_IMAGE_MAX&&!(base&(SF_IMAGE_MAX-1)));}
#define SF_MAX_TASKS 32
#define SF_MAX_THREADS 64
#define SF_MAX_CPUS 8
#define SF_THREAD_LIMIT_MAX 32
#define SF_THREAD_STACK_BASE 0x40300000ull
#define SF_THREAD_STACK_BYTES 65536u
#define SF_CPU_WINDOW_TICKS 100u
#define SF_MSG_BYTES 4096
#define SF_ARGUMENT_MAX 8u
#define SF_ENVIRONMENT_MAX 8u
#define SF_ARGUMENT_BYTES 512u
/* SYS_TASK_STATUS must not confuse a live task with any int32_t exit code. */
#define SF_TASK_RUNNING 0x100000000ll
enum { SF_OK=0, SF_INVALID=-1, SF_DENIED=-2, SF_BAD_FORMAT=-3,
 SF_BAD_SIGNATURE=-4, SF_ABI_MISMATCH=-5, SF_DEPENDENCY=-6,
 SF_BUSY=-7, SF_DRAINING=-8, SF_TIMEOUT=-9, SF_DEVICE_GONE=-10,
 SF_NOT_SEEKABLE=-11, SF_STALE_HANDLE=-12, SF_REVOKED=-13,
 SF_RESTART_REQUIRED=-14, SF_WOULD_BLOCK=-15, SF_NOT_FOUND=-16,
 SF_READ_ONLY=-17, SF_NO_SPACE=-18, SF_EVENT_OVERFLOW=-19, SF_COMMIT_UNCERTAIN=-20,
 SF_VERSION_REJECTED=-21 };
#define SF_DEVICE_WRITE 1u
#define SF_DEVICE_ISOLATED 2u /* Kernel-reported result; never a requested right. */
#define SF_DEVICE_MODERN 4u
#define SF_DEVICE_MMIO 0x51000000u
enum { SYS_EXIT, SYS_YIELD, SYS_LOG, SYS_TICKS, SYS_EP_CREATE,
 SYS_SEND, SYS_RECV, SYS_SPAWN, SYS_KILL, SYS_TASK_STATUS,
 SYS_CONSOLE_READ, SYS_SHUTDOWN, SYS_CAP_DERIVE, SYS_CAP_CLOSE,
 SYS_MAP_TEST, SYS_CAP_REVOKE, SYS_PCI_READ, SYS_PORT_IO, SYS_DEVICE_RELEASE, SYS_DMA_STATUS, SYS_TASK_PRIORITY, SYS_TASK_CAP_LIMIT, SYS_SEND_CANCEL, SYS_SPAWN_LIMITED, SYS_TASK_PAGE_LIMIT, SYS_IPC_STATS };
#define SYS_THREAD_CREATE 26
#define SYS_THREAD_LIMIT 27
#define SYS_TASK_CPU_BUDGET 28
/* 设备中断：取得当前授权设备的 INTx（写入 IOAPIC 重定向），释放，以及
 * 阻塞等待一次中断（参数为超时滴答数，0 表示无限等待）。 */
#define SYS_IRQ_ACQUIRE 29
#define SYS_IRQ_RELEASE 30
#define SYS_IRQ_WAIT 31
/* Framebuffer：PID1 授权目标进程，目标进程再把显示内存映射到 SF_FB。 */
#define SYS_FB_GRANT 32
#define SYS_FB_MAP 33
/* PS/2 指针轮询：参数 a=1 表示回到屏幕中心并清空积压；否则返回
 * bit31 事件标记 | buttons[26..28] | y[13..25] | x[0..12]，
 * 无新事件返回 SF_WOULD_BLOCK。仅 PID1 可用。 */
#define SYS_POINTER_READ 34
/* 匿名映射与共享内存：内核只提供机制，策略由 sf-init/sf-native 决定。 */
#define SYS_MMAP 35
#define SYS_MUNMAP 36
#define SYS_SHM_CREATE 37
#define SYS_SHM_MAP 38
#define SYS_SHM_GRANT 39
#define SYS_SHM_CLOSE 40
enum { SF_PRIORITY_MIN=0, SF_PRIORITY_DEFAULT=1, SF_PRIORITY_MAX=3, SF_PRIORITY_AGING_TICKS=4,
 SF_PRIORITY_QUERY=0, SF_PRIORITY_SET=1 };
enum { SF_CAP_LIMIT_MAX=32, SF_LIMIT_QUERY=0, SF_LIMIT_SET=1, SF_LIMIT_USED=2 };
enum { SF_PAGE_LIMIT_MAX=4096 };
enum { SF_IPC_STATS_VERSION=1 };

enum { RIGHT_SEND=1, RIGHT_RECV=2, RIGHT_DELEGATE=4 };
enum { OP_ECHO=1, OP_ECHO_DIAGNOSTIC=2, OP_OPEN=10, OP_READ, OP_WRITE, OP_CLOSE, OP_SEEK,
 OP_POLL, OP_CREATE, OP_DESCRIBE, OP_ENUMERATE, OP_OPENAT, OP_ROOT, OP_TRUNCATE, OP_UNLINK, OP_STAT, OP_RMDIR, OP_DUP=25, OP_PREAD=26, OP_ENUMERATE_AT=27, OP_HELLO=30, OP_NATIVE_PREPARE=40,
 OP_NATIVE_STAGE, OP_NATIVE_HEALTH, OP_NATIVE_DIAGNOSTIC, OP_BLOCK_INFO=50, OP_BLOCK_READ, OP_BLOCK_WRITE, OP_BLOCK_STOP,
 OP_BLOCK_FLUSH, OP_BLOCK_WRITE_RANGE, OP_BLOCK_DMA_PROBE,
 OP_NET_INFO=60, OP_NET_PING, OP_NET_UDP, OP_NET_TCP,
 OP_NET_DHCP=64, OP_NET_OPEN, OP_NET_SEND, OP_NET_RECV, OP_NET_CLOSE, OP_NET_LIST,
 OP_MODULE_STATUS=70, OP_MODULE_LIST, OP_MODULE_START, OP_MODULE_PREPARE,
 OP_MODULE_CANCEL, OP_MODULE_STOP, OP_MODULE_UNLOAD, OP_MODULE_ACQUIRE,
 OP_MODULE_RELEASE,
 OP_PERSIST_ATTACH_SEND=80, OP_PERSIST_ATTACH_REPLY, OP_PERSIST_MOUNT,
 OP_PERSIST_READ, OP_PERSIST_WRITE, OP_PERSIST_TRUNCATE, OP_PERSIST_STAGE,
 OP_PERSIST_INSTALL, OP_PERSIST_BEGIN, OP_PERSIST_FINISH, OP_PERSIST_REVOKE,
 OP_PERSIST_STATUS, OP_PERSIST_PACKAGE, OP_PERSIST_PREVIEW, OP_PERSIST_REVOKED,
 OP_PERSIST_DRIVER_DOWN,
 OP_FS_ATTACH_SEND=95, OP_FS_ATTACH_REPLY, OP_FS_MOUNT, OP_FS_ENTRY,
 OP_FS_FIND, OP_FS_READ, OP_FS_STOP, OP_FS_BLOCK_READ,
 /* Read-only mounted-volume metadata; never repeats trial recovery. */
 OP_PERSIST_NOTES_SIZE=103 };
/* 合成器输入与生命周期：sf-init 把串口按键转发给 sf-wm。
 * OP_WM_REGISTER 供 GUI 应用登记目录，data 为 id\0title\0。
 * OP_WM_POINTER 的 handle 携带 SYS_POINTER_READ 的打包指针状态。 */
enum { OP_WM_INPUT=110, OP_WM_STOP, OP_WM_STATUS, OP_WM_REGISTER, OP_WM_POINTER };
enum { SF_FILE_READ_ONLY=1, SF_FILE_MOUNTED=2, SF_FILE_SEEKABLE=4 };
typedef struct {uint32_t bytes,type,mode,flags;uint64_t size,offset;} SfFileStat;
_Static_assert(sizeof(SfFileStat)==32,"file stat ABI");

typedef struct {
 uint32_t size,mode,owner,faults; uint64_t last_address,probe_address;
 uint32_t source,reason,guard_intact,active_mapping;
} SfDmaStatus;
_Static_assert(sizeof(SfDmaStatus)==48,"DMA status ABI");
typedef struct {
 uint32_t size; uint16_t protocol_major,protocol_minor;
 uint32_t opcode,flags; uint64_t request_id;
 uint32_t payload_bytes,cap_count; uint64_t deadline_ns;
} SfIpcHeader;
_Static_assert(sizeof(SfIpcHeader)==40,"IPC ABI");
typedef struct {uint32_t size,version;uint64_t send_bytes,receive_bytes;} SfIpcStats;
_Static_assert(sizeof(SfIpcStats)==24,"IPC counters ABI");
typedef struct { SfIpcHeader h; int64_t status; uint64_t handle,offset;
 uint32_t length,mode; uint8_t data[SF_MSG_BYTES]; } SfMessage;
typedef struct {uint32_t size,flags;uint64_t entry,argument;} SfThreadRequest;
_Static_assert(sizeof(SfThreadRequest)==24,"thread request ABI");
/* Framebuffer 查询结果（SYS_FB_MAP 返回；pitch 为字节步长）。 */
typedef struct {
 uint32_t size,width,height,pitch,bpp,reserved;
 uint64_t bytes;
} SfFbInfo;
_Static_assert(sizeof(SfFbInfo)==32,"framebuffer info ABI");
typedef struct {
 uint32_t size; uint16_t abi_major,abi_minor;
 uint64_t bundle, bundle_bytes;
 uint32_t pid,manager,reply,service;
 uint32_t stdin_cap,stdout_cap,stderr_cap,root_cap;
 uint32_t argc,argv_offset,envc,env_offset;
 uint64_t dma_virtual,dma_physical;
 uint32_t device_io; uint16_t device_bdf,device_rights;
} SfBootArgs;
_Static_assert(sizeof(SfBootArgs)==96,"BootArgs ABI");
typedef struct { uint64_t physical,rva,file_bytes,memory_bytes; uint32_t flags,reserved; } SfBootSegment;
typedef struct {
 uint32_t magic,boot_type;
 uint64_t framebuffer_base,framebuffer_size;
 uint32_t width,height,pitch,bpp;
 uint64_t memory_map,memory_map_size,memory_map_descriptor_size,memory_map_entry_count;
 uint64_t acpi_rsdp,efi_runtime_services,total_memory,initrd_addr,initrd_size;
 uint32_t size,version,init_segment_count,reserved;
 uint64_t init_entry,bundle,bundle_bytes;
 SfBootSegment init_segments[8];
} SfBootInfo;
typedef struct { uint64_t rva,bytes,data; uint32_t flags,reserved; } SfLoadSegment;
typedef struct { uint32_t count,bytes,env_count,reserved; uint8_t data[SF_ARGUMENT_BYTES]; } SfArguments;
typedef struct {
 uint32_t size,count; uint64_t entry,image_base;
 uint32_t grant_manager,grant_service,service_rights,grant_bundle;
 uint32_t grant_device,device_rights;
 SfLoadSegment segments[8];
 SfArguments arguments;
} SfLoadPlan;
typedef struct {uint64_t rva,memory,offset,bytes;uint32_t flags,reserved;} SfNativeSegment;
typedef struct {uint64_t entry,image_base,relocation_offset;uint32_t count,profile,relocation_count,reserved;SfNativeSegment segments[5];} SfNativePlan;
_Static_assert(sizeof(SfNativePlan)<=SF_MSG_BYTES,"native plan fits copied IPC");
void *memcpy(void *,const void *,size_t);
void *memset(void *,int,size_t);
int memcmp(const void *,const void *,size_t);
size_t strlen(const char *);
int strcmp(const char *,const char *);
int strncmp(const char *,const char *,size_t);
#endif
