/* SpiritFoxOS - Multiboot2协议定义
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
#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

/* Multiboot2魔数 */
#define MULTIBOOT2_MAGIC 0x36D76289

/* Multiboot2标签类型 */
#define MULTIBOOT2_TAG_END              0
#define MULTIBOOT2_TAG_CMDLINE          1
#define MULTIBOOT2_TAG_BOOTLOADER_NAME  2
#define MULTIBOOT2_TAG_MODULE           3
#define MULTIBOOT2_TAG_BASIC_MEMINFO    4
#define MULTIBOOT2_TAG_BOOTDEV          5
#define MULTIBOOT2_TAG_MMAP             6
#define MULTIBOOT2_TAG_FRAMEBUFFER      8

/* 内存类型
#define MULTIBOOT2_MEMORY_AVAILABLE     1
#define MULTIBOOT2_MEMORY_RESERVED      2
#define MULTIBOOT2_MEMORY_ACPI_RECLAIMABLE 3
#define MULTIBOOT2_MEMORY_NVS           4
#define MULTIBOOT2_MEMORY_UNUSABLE      5

/* Multiboot2标签头 */
struct multiboot2_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

/* 基本内存信息标签 */
struct multiboot2_tag_basic_meminfo {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
} __attribute__((packed));

/* 内存映射条目 */
struct multiboot2_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

/* 内存映射标签 */
struct multiboot2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot2_mmap_entry entries[];
} __attribute__((packed));

/* 帧缓冲区标签 */
struct multiboot2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[];
} __attribute__((packed));

/* 遍历Multiboot2标签 */
#define FOR_EACH_TAG(tag, mbi) \
    for ((tag) = (struct multiboot2_tag *)((uint8_t *)(mbi) + 8); \
         (tag)->type != MULTIBOOT2_TAG_END; \
         (tag) = (struct multiboot2_tag *)((uint8_t *)(tag) + (((tag)->size + 7) & ~7)))

#endif /* MULTIBOOT2_H */
