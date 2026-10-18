/* SpiritFoxOS 模块 SDK。ABI 1.2：仅适用于受信任的内核态模块。 */
#ifndef SPIRITFOX_SFMOD_H
#define SPIRITFOX_SFMOD_H

#include <stddef.h>
#include <stdint.h>

#define SFM_ABI 0x00010002u
#define SFM_MAGIC 0x004d4653u
#define SFM_PAYLOAD 4096u
#define SFM_MAX_MODULES 16u
#define SFM_MAX_DEPS 8u
#define SFM_QUEUE_DEPTH 4u
#define SFM_TRUSTED 1u

enum sfm_operation
{
    SFM_PING = 3,
    SFM_FILE_OPEN = 0x1001,
    SFM_FILE_READ = 0x1003,
    SFM_FILE_WRITE = 0x1004,
    SFM_FILE_SEEK = 0x1005,
    SFM_FILE_STAT = 0x1007,
    SFM_FILE_TRUNCATE = 0x1008,
    SFM_FILE_MKDIR = 0x1010,
    SFM_FILE_READDIR = 0x1011,
    SFM_FILE_RETAIN = 0x1012,
    SFM_FILE_RELEASE = 0x1013,
    SFM_FILE_REMOVE = 0x1014,
    SFM_FILE_SYMLINK = 0x1015,
    SFM_FS_ROOT = 0x2000
};
#define SFM_CREATE 1u
#define SFM_NODE_FILE 1u
#define SFM_NODE_DIR 2u
#define SFM_NODE_SYMLINK 3u
#define SFM_NODE_CHARDEV 4u
#define SFM_ROUTE_TMPFS 0u
#define SFM_ROUTE_DEVFS 1u
#define SFM_ROUTE_COUNT 2u
#define SFM_FS_INSTANCES 3u
#define SFM_FILE_NODES 64u
#define SFM_FILE_CAPACITY 65536u
#define SFM_TEMP_CAPACITY 1048576u
#define SFM_FILE_NAME 64u
#define SFM_FILE_PATH 256u

typedef struct sfm_message
{
    uint64_t src;
    uint64_t dst;
    uint32_t type;
    uint32_t sequence;
    uint32_t size;
    uint32_t flags;
    int64_t result;
    uint64_t offset;
    uint64_t handle;
    uint64_t route; /* Fixed backend selector, not an endpoint or capability. */
    uint8_t data[SFM_PAYLOAD];
} sfm_message_t;

typedef struct sfm_metadata
{
    uint32_t magic;
    uint32_t abi;
    uint32_t header_size;
    uint32_t flags;
    char name[32];
    char version[16];
    uint64_t permissions;
    uint32_t priority;
    uint32_t reserved;
    uint64_t module_id;
    uint64_t reserved2;
} sfm_metadata_t;

typedef int (*sfm_handler_t)(const sfm_message_t *, sfm_message_t *);

typedef struct sfm_api
{
    uint32_t abi;
    uint32_t size;
    void *(*alloc)(size_t);
    void (*free)(void *);
    void (*log)(const char *);
    uint64_t (*endpoint_create)(uint32_t owner, const char *, sfm_handler_t);
    int (*endpoint_destroy)(uint32_t owner, uint64_t);
    uint64_t (*lookup)(const char *);
    int (*call)(uint64_t, const sfm_message_t *, sfm_message_t *);
    int (*send)(uint64_t, const sfm_message_t *);
    int (*recv)(uint32_t owner, uint64_t, sfm_message_t *);
} sfm_api_t;

typedef struct sfm_entry
{
    int (*init)(const sfm_api_t *, uint32_t owner);
    void (*exit)(void);
} sfm_entry_t;

#define SFM_MODULE(module_name, init_fn, exit_fn) \
    __attribute__((section(".sfmod_meta"), used, aligned(8))) \
    static const sfm_metadata_t sfm_meta = { \
        .magic = SFM_MAGIC, .abi = SFM_ABI, .header_size = sizeof(sfm_metadata_t), \
        .flags = SFM_TRUSTED, .name = module_name, .version = "0.1.0" }; \
    __attribute__((section(".sfmod_init"), used, aligned(8))) \
    static const sfm_entry_t sfm_entry = { init_fn, exit_fn }

#define SFM_DEPENDS(...) \
    __attribute__((section(".sfmod_deps"), used, aligned(1))) \
    static const char sfm_dependencies[][32] = { __VA_ARGS__ }

_Static_assert(sizeof(sfm_metadata_t) == 96, "sfmod metadata ABI");
_Static_assert(offsetof(sfm_message_t, data) == 64, "sfmod message ABI");
_Static_assert(offsetof(sfm_message_t, route) == 56, "sfmod route ABI");
_Static_assert(sizeof(sfm_entry_t) == 16, "sfmod entry ABI");

/* 清零和复制不依赖宿主 libc，也不把裸指针写入协议。 */
static inline void sfm_clear(void *address, size_t size)
{
    uint8_t *p = address;
    while (size--) *p++ = 0;
}

static inline void sfm_copy(void *target, const void *source, size_t size)
{
    uint8_t *d = target;
    const uint8_t *s = source;
    while (size--) *d++ = *s++;
}

static inline int sfm_name_equal(const char *a, const char *b)
{
    for (size_t i = 0; i < 32; i++)
    {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 0;
}

#endif
