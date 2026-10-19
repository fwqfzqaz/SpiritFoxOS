/* Boot-time namespace protocol. Logical owners are not security capabilities. */
#ifndef SFM_DEVICE_H
#define SFM_DEVICE_H
#include <stdint.h>
#include <stddef.h>
#define SFM_DEV_VERSION 1u
#define SFM_DEV_MAX 64u
#define SFM_DEV_REGISTER 0x4000u
#define SFM_DEV_SEAL 0x4001u
#define SFM_DEV_LOOKUP 0x4002u
#define SFM_DEV_ENUM 0x4003u
#define SFM_DEV_UNREGISTER 0x4004u
#define SFM_DEV_BOOT 1u
#define SFM_DEV_SPECIAL 2u
#define SFM_DEV_DIRECTORY 0u
#define SFM_DEV_TMPFS 1u
#define SFM_DEV_CHAR 2u
#define SFM_DEV_BLOCK 3u
#define SFM_DEV_CONSOLE 4u
#define SFM_DEV_RDEV_KNOWN 1u
typedef struct sfm_device_record {
    uint32_t version, size;
    uint64_t handle, parent, backend_handle, rdev, bytes;
    uint32_t owner, type, mode, backend, flags, reserved;
    char path[256];
} sfm_device_record_t;
_Static_assert(offsetof(sfm_device_record_t, path) == 72, "device record header ABI");
_Static_assert(sizeof(sfm_device_record_t) == 328, "device record ABI");
#endif
