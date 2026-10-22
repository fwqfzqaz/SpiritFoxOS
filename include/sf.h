#ifndef SF_H
#define SF_H
#include <stdint.h>
#include <stddef.h>
#define SF_ABI 1
#define SF_PAGE 4096ull
#define SF_BASE 0x40000000ull
#define SF_IMAGE_MAX 0x200000ull
#define SF_STACK 0x41010000ull
#define SF_ARGS 0x42000000ull
#define SF_BUNDLE 0x60000000ull
#define SF_MAX_TASKS 12
#define SF_MSG_BYTES 256
enum { SF_OK=0, SF_INVALID=-1, SF_DENIED=-2, SF_BAD_FORMAT=-3,
 SF_BAD_SIGNATURE=-4, SF_ABI_MISMATCH=-5, SF_DEPENDENCY=-6,
 SF_BUSY=-7, SF_DRAINING=-8, SF_TIMEOUT=-9, SF_DEVICE_GONE=-10,
 SF_NOT_SEEKABLE=-11, SF_STALE_HANDLE=-12, SF_REVOKED=-13,
 SF_RESTART_REQUIRED=-14, SF_WOULD_BLOCK=-15, SF_NOT_FOUND=-16,
 SF_READ_ONLY=-17, SF_NO_SPACE=-18, SF_EVENT_OVERFLOW=-19 };
enum { SYS_EXIT, SYS_YIELD, SYS_LOG, SYS_TICKS, SYS_EP_CREATE,
 SYS_SEND, SYS_RECV, SYS_SPAWN, SYS_KILL, SYS_TASK_STATUS,
 SYS_CONSOLE_READ, SYS_SHUTDOWN, SYS_CAP_DERIVE, SYS_CAP_CLOSE,
 SYS_MAP_TEST, SYS_CAP_REVOKE };
enum { RIGHT_SEND=1, RIGHT_RECV=2, RIGHT_DELEGATE=4 };
enum { OP_ECHO=1, OP_OPEN=10, OP_READ, OP_WRITE, OP_CLOSE, OP_SEEK,
 OP_POLL, OP_CREATE, OP_DESCRIBE, OP_ENUMERATE, OP_HELLO=30, OP_NATIVE_PREPARE=40 };
typedef struct {
 uint32_t size; uint16_t protocol_major,protocol_minor;
 uint32_t opcode,flags; uint64_t request_id;
 uint32_t payload_bytes,cap_count; uint64_t deadline_ns;
} SfIpcHeader;
_Static_assert(sizeof(SfIpcHeader)==40,"IPC ABI");
typedef struct { SfIpcHeader h; int64_t status; uint64_t handle,offset;
 uint32_t length,mode; uint8_t data[SF_MSG_BYTES]; } SfMessage;
typedef struct {
 uint32_t size; uint16_t abi_major,abi_minor;
 uint64_t bundle, bundle_bytes;
 uint32_t pid,manager,reply,service;
 uint32_t stdin_cap,stdout_cap,stderr_cap,root_cap;
 uint32_t argc,argv_offset,envc,env_offset;
 uint8_t reserved[24];
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
typedef struct {
 uint32_t size,count; uint64_t entry;
 uint32_t grant_manager,grant_service,service_rights,grant_bundle;
 SfLoadSegment segments[8];
} SfLoadPlan;
typedef struct {uint64_t rva,memory,offset,bytes;uint32_t flags,reserved;} SfNativeSegment;
typedef struct {uint64_t entry;uint32_t count,reserved;SfNativeSegment segments[5];} SfNativePlan;
void *memcpy(void *,const void *,size_t);
void *memset(void *,int,size_t);
int memcmp(const void *,const void *,size_t);
size_t strlen(const char *);
int strcmp(const char *,const char *);
#endif
