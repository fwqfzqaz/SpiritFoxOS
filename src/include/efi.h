#ifndef EFI_H
#define EFI_H

#include <stdint.h>

/* ============================================================
 * UEFI Type Definitions (simplified for x86_64)
 * Based on UEFI Specification 2.10
 *
 * CRITICAL: UEFI on x86_64 uses Microsoft x64 calling convention.
 * All EFI entry points and service function pointers must use
 * the __attribute__((ms_abi)) calling convention (EFIAPI macro).
 * ============================================================ */

/* EFIAPI: Microsoft x64 calling convention for UEFI */
#define EFIAPI __attribute__((ms_abi))

/* char16_t equivalent for UEFI */
typedef uint16_t char16_t;

/* EFI status codes */
typedef int64_t EFI_STATUS;
#define EFI_SUCCESS             0
#define EFI_LOAD_ERROR          (1 | (1LL << 63))
#define EFI_INVALID_PARAMETER   (2 | (1LL << 63))
#define EFI_UNSUPPORTED         (3 | (1LL << 63))
#define EFI_OUT_OF_RESOURCES    (4 | (1LL << 63))
#define EFI_BUFFER_TOO_SMALL    (5 | (1LL << 63))
#define EFI_NOT_FOUND           (6 | (1LL << 63))

/* Handle types */
typedef void *EFI_HANDLE;
typedef uint64_t EFI_EVENT;
typedef uintptr_t EFI_PHYSICAL_ADDRESS;
typedef uintptr_t EFI_VIRTUAL_ADDRESS;

/* Console handle types (must be defined before EFI_SYSTEM_TABLE) */
typedef void *EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef void *EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* Memory types */
#define EFI_RESERVED_MEMORY_TYPE        0
#define EFI_LOADER_CODE                 1
#define EFI_LOADER_DATA                 2
#define EFI_BOOT_SERVICES_CODE          3
#define EFI_BOOT_SERVICES_DATA          4
#define EFI_RUNTIME_SERVICES_CODE       5
#define EFI_RUNTIME_SERVICES_DATA       6
#define EFI_CONVENTIONAL_MEMORY         7
#define EFI_UNUSABLE_MEMORY             8
#define EFI_ACPI_RECLAIM_MEMORY         9
#define EFI_ACPI_MEMORY_NVS            10
#define EFI_MEMORY_MAPPED_IO           11
#define EFI_MEMORY_MAPPED_IO_PORT_SPACE 12
#define EFI_PAL_CODE                   13
#define EFI_PERSISTENT_MEMORY           14

/* Memory attribute flags */
#define EFI_MEMORY_UC      0x0000000000000001ULL
#define EFI_MEMORY_WC      0x0000000000000002ULL
#define EFI_MEMORY_WT      0x0000000000000004ULL
#define EFI_MEMORY_WB      0x0000000000000008ULL
#define EFI_MEMORY_UCE     0x0000000000000010ULL
#define EFI_MEMORY_WP      0x0000000000001000ULL
#define EFI_MEMORY_RP      0x0000000000002000ULL
#define EFI_MEMORY_XP      0x0000000000004000ULL
#define EFI_MEMORY_NV      0x0000000000008000ULL
#define EFI_MEMORY_MORE_RELIABLE 0x0100000000000000ULL
#define EFI_MEMORY_RO      0x0200000000000000ULL
#define EFI_MEMORY_SP      0x0400000000000000ULL
#define EFI_MEMORY_CPU_CRYPTO 0x0800000000000000ULL
#define EFI_MEMORY_RUNTIME 0x8000000000000000ULL
#define EFI_MEMORY_BS      0x0080000000000000ULL

/* GUID structure */
typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} EFI_GUID;

/* Standard EFI GUIDs */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}}

/* Table header */
typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
} EFI_TABLE_HEADER;

/* Memory descriptor */
typedef struct {
    uint32_t type;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} EFI_MEMORY_DESCRIPTOR;

/* EFI Input Key */
typedef struct {
    uint16_t scan_code;
    char16_t unicode_char;
} EFI_INPUT_KEY;

/* Graphics Output Protocol - Mode Information */
typedef struct {
    uint32_t version;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixel_format;
    uint32_t pixel_information_mask;
    uint32_t pixel_shift;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

/* Pixel formats */
#define PIXEL_RED_GREEN_BLUE_RESERVED_8BIT_PER_COLOR  0
#define PIXEL_BLUE_GREEN_RED_RESERVED_8BIT_PER_COLOR   1
#define PIXEL_BIT_MASK                                  2
#define PIXEL_BLT_ONLY                                  3

/* GOP mode structure */
typedef struct {
    uint32_t max_mode;
    uint32_t mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    uint64_t size_of_info;
    EFI_PHYSICAL_ADDRESS frame_buffer_base;
    uint32_t frame_buffer_size;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

/* Graphics Output Protocol (simplified - function pointers as uint64_t) */
typedef struct {
    uint64_t query_mode;
    uint64_t set_mode;
    uint64_t blt;
    void *mode;  /* EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE * */
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* Simple text output protocol (function pointers as uint64_t) */
typedef struct {
    uint64_t reset;
    uint64_t output_string;
    uint64_t test_string;
    uint64_t query_mode;
    uint64_t set_mode;
    uint64_t set_attribute;
    uint64_t clear_screen;
    uint64_t set_cursor_position;
    uint64_t enable_cursor;
    void *mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_T;

/* Loaded Image Protocol (simplified) */
typedef struct {
    uint32_t revision;
    void *parent_handle;
    void *device_handle;
    void *file_path;
    void *reserved;
    uint32_t load_options_size;
    void *load_options;
    void *image_base;
    uint64_t image_size;
    void *image_code_type;
    void *image_data_type;
    void *unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* Boot Services table (simplified - function pointers as uint64_t)
 * IMPORTANT: Must match UEFI Spec 2.10 layout exactly!
 * No reserved/padding fields between protocol handler services! */
typedef struct {
    EFI_TABLE_HEADER hdr;

    /* Task Priority Services */
    void *raise_tpl;
    void *restore_tpl;

    /* Memory Services */
    uint64_t allocate_pages;
    uint64_t free_pages;
    uint64_t get_memory_map;
    uint64_t allocate_pool;
    uint64_t free_pool;

    /* Event & Timer Services */
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;

    /* Protocol Handler Services */
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    uint64_t handle_protocol;
    void *register_protocol_notify;       /* +0xA0 (was: reserved_service - REMOVED!) */
    uint64_t locate_handle;               /* +0xA8 */
    uint64_t locate_protocol;             /* +0xB0 (was: 0xB8 - FIXED!) */
    void *open_protocol;
    void *close_protocol;
    void *protocols_per_page;
    void *register_handle;
    void *unregister_handle;

    /* Image Services */
    uint64_t load_image;
    uint64_t start_image;
    void *exit;
    void *unload_image;
    uint64_t exit_boot_services;
} EFI_BOOT_SERVICES;

/* Runtime Services table */
typedef struct {
    EFI_TABLE_HEADER hdr;
    void *get_time;
    void *set_time;
    void *get_wakeup_time;
    void *set_wakeup_time;
    void *set_virtual_address_map;
    void *convert_pointer;
    uint64_t get_variable;
    uint64_t get_next_variable_name;
    uint64_t set_variable;
    void *get_next_high_mono_count;
    void *reset_system;
} EFI_RUNTIME_SERVICES;

/* Configuration Table entry */
typedef struct {
    EFI_GUID vendor_guid;
    void *vendor_table;
} EFI_CONFIGURATION_TABLE;

/* ACPI RSDP signatures */
#define ACPI_20_RSDP_GUID \
    {0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}}

/* System Table (defined last, after all dependent types)
 * IMPORTANT: Must match UEFI Spec 2.10 layout exactly!
 * Each Handle field is 8 bytes on x86_64 (EFI_HANDLE = void*). */
typedef struct {
    EFI_TABLE_HEADER hdr;                        /* +0x00 (24 bytes) */
    uint16_t *firmware_vendor;                   /* +0x18  (8 bytes)  */
    uint32_t firmware_revision;                  /* +0x20  (4 bytes)  */
    uint32_t _pad1;                              /* +0x24  (4 bytes pad)*/
    EFI_HANDLE console_in_handle;                /* +0x28  (8 bytes)  */
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *con_in;      /* +0x30  (8 bytes)  */
    EFI_HANDLE console_out_handle;               /* +0x38  (8 bytes)  */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_T *con_out;  /* +0x40  (8 bytes)  */
    EFI_HANDLE standard_error_handle;            /* +0x48  (8 bytes)  */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_T *err_out;  /* +0x50  (8 bytes)  */
    EFI_RUNTIME_SERVICES *runtime;               /* +0x58  (8 bytes)  */
    EFI_BOOT_SERVICES *boot;                     /* +0x60  (8 bytes)  */
    uint64_t number_of_table_entries;            /* +0x68  (8 bytes)  */
    EFI_CONFIGURATION_TABLE *configuration_table;/* +0x70  (8 bytes)  */
} EFI_SYSTEM_TABLE;

#endif /* EFI_H */
