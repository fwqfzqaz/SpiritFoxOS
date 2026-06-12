/* SpiritFoxOS - UEFI类型定义
 * Copyright (C) 2025 SpiritFoxOS Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef EFI_H
#define EFI_H

#include <stdint.h>

/* ============================================================
 * UEFI类型定义（x86_64简化版）
 * 基于UEFI规范 2.10
 *
 * 关键说明：x86_64上的UEFI使用Microsoft x64调用约定。
 * 所有UEFI入口点和服务函数指针必须使用
 * __attribute__((ms_abi)) 调用约定（EFIAPI宏）。
 * ============================================================ */

/* EFIAPI：用于UEFI的Microsoft x64调用约定 */
#define EFIAPI __attribute__((ms_abi))

/* UEFI的char16_t等价类型 */
typedef uint16_t char16_t;

/* EFI状态码 */
typedef int64_t EFI_STATUS;
#define EFI_SUCCESS             0
#define EFI_LOAD_ERROR          (1 | (1LL << 63))
#define EFI_INVALID_PARAMETER   (2 | (1LL << 63))
#define EFI_UNSUPPORTED         (3 | (1LL << 63))
#define EFI_OUT_OF_RESOURCES    (4 | (1LL << 63))
#define EFI_BUFFER_TOO_SMALL    (5 | (1LL << 63))
#define EFI_NOT_FOUND           (6 | (1LL << 63))

/* 句柄类型 */
typedef void *EFI_HANDLE;
typedef uint64_t EFI_EVENT;
typedef uintptr_t EFI_PHYSICAL_ADDRESS;
typedef uintptr_t EFI_VIRTUAL_ADDRESS;

/* 控制台句柄类型（必须在 之前定义）
typedef void *EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef void *EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* 内存类型 */
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

/* 内存属性标志 */
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

/* GUID结构体 */
typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} EFI_GUID;

/* 标准EFI GUID定义 */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}}

/* 表头 */
typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
} EFI_TABLE_HEADER;

/* 内存描述符 */
typedef struct {
    uint32_t type;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} EFI_MEMORY_DESCRIPTOR;

/* EFI 输入按键 */
typedef struct {
    uint16_t scan_code;
    char16_t unicode_char;
} EFI_INPUT_KEY;

/* 图形输出协议 - 模式信息 */
typedef struct {
    uint32_t version;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixel_format;
    uint32_t pixel_information_mask;
    uint32_t pixel_shift;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

/* 像素格式 */
#define PIXEL_RED_GREEN_BLUE_RESERVED_8BIT_PER_COLOR  0
#define PIXEL_BLUE_GREEN_RED_RESERVED_8BIT_PER_COLOR   1
#define PIXEL_BIT_MASK                                  2
#define PIXEL_BLT_ONLY                                  3

/* GOP模式结构体 */
typedef struct {
    uint32_t max_mode;
    uint32_t mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    uint64_t size_of_info;
    EFI_PHYSICAL_ADDRESS frame_buffer_base;
    uint32_t frame_buffer_size;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

/* 图形输出协议（简化版 - 函数指针用uint64_t表示） */
typedef struct {
    uint64_t query_mode;
    uint64_t set_mode;
    uint64_t blt;
    void *mode;  /* EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE * */
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* 简单文本输出协议（函数指针用 uint64_t 表示） */
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

/* 已加载映像协议（简化版） */
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

/* 引导服务表（简化版 - 函数指针用uint64_t表示）
 * 重要说明：必须与UEFI规范2.10的布局完全匹配！
 * 协议处理服务之间无保留/填充字段！ */
typedef struct {
    EFI_TABLE_HEADER hdr;

    /* 任务优先级服务 */
    void *raise_tpl;
    void *restore_tpl;

    /* 内存服务 */
    uint64_t allocate_pages;
    uint64_t free_pages;
    uint64_t get_memory_map;
    uint64_t allocate_pool;
    uint64_t free_pool;

    /* 事件与定时器服务 */
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;

    /* 协议处理服务 */
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    uint64_t handle_protocol;
    void *register_protocol_notify;       /* +0xA0（原为：reserved_service - 已移除！） */
    uint64_t locate_handle;               /* +0xA8 */
    uint64_t locate_protocol;             /* +0xB0（原为：0xB8 - 已修正！） */
    void *open_protocol;
    void *close_protocol;
    void *protocols_per_page;
    void *register_handle;
    void *unregister_handle;

    /* 映像服务 */
    uint64_t load_image;
    uint64_t start_image;
    void *exit;
    void *unload_image;
    uint64_t exit_boot_services;
} EFI_BOOT_SERVICES;

/* 运行时服务表 */
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

/* 配置表条目 */
typedef struct {
    EFI_GUID vendor_guid;
    void *vendor_table;
} EFI_CONFIGURATION_TABLE;

/* ACPI RSDP签名 */
#define ACPI_20_RSDP_GUID \
    {0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}}

/* 系统表（在所有依赖类型之后定义）
 * 重要说明：必须与 UEFI 规范 2.10 的布局完全匹配！
 * 在 x86_64 上每个 Handle 字段为 8 字节（EFI_HANDLE = void*）。 */
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
