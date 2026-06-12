/* SpiritFoxOS - 启动信息结构
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
#ifndef BOOTINFO_H
#define BOOTINFO_H

#include <stdint.h>

/* ============================================================
 * 统一启动信息结构
 * 用于 UEFI 启动路径，替代 Multiboot2 信息。
 * 包含内核从引导加载程序/固件获取的所有数据。
 * ============================================================ */

/* 内存映射条目 - 兼容UEFI和Multiboot2布局 */
typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} bootinfo_mmap_entry_t;

/* 内存类型（匹配UEFI和通用约定） */
#define BOOTINFO_MEM_AVAILABLE       1
#define BOOTINFO_MEM_RESERVED        2
#define BOOTINFO_MEM_ACPI_RECLAIMABLE 3
#define BOOTINFO_MEM_NVS             4
#define BOOTINFO_MEM_UNUSABLE        5

/* 帧缓冲区类型 */
#define BOOTINFO_FB_RGB     1   /* RGB 帧缓冲区 */
#define BOOTINFO_FB_TEXT    2   /* 文本模式（无帧缓冲区） */

/* 帧缓冲区信息 */
typedef struct {
    uint64_t address;          /* 帧缓冲区物理地址 */
    uint32_t pitch;            /* 每行字节数 */
    uint32_t width;            /* 水平分辨率 */
    uint32_t height;           /* 垂直分辨率 */
    uint8_t  bpp;              /* 每像素位数（16/24/32） */
    uint8_t  type;             /* 帧缓冲区类型：RGB或文本 */
    uint8_t  reserved[2];
} bootinfo_fb_t;

/* ACPI RSDP指针 */
typedef struct {
    uint64_t address;          /* RSDP物理地址 */
    uint8_t  revision;         /* ACPI版本（1或2） */
} bootinfo_acpi_t;

/* 统一启动信息 - 传递给kernel_main */
typedef struct {
    /* 用于验证的魔数 */
    uint32_t magic;

    /* 内存映射 */
    uint64_t mmap_addr;        /* 内存映射条目物理地址 */
    uint64_t mmap_entry_count; /* 内存映射条目数量 */
    uint64_t mmap_entry_size;  /* 每个条目大小（字节） */

    /* 帧缓冲区 */
    bootinfo_fb_t framebuffer;

    /* ACPI */
    bootinfo_acpi_t acpi;

    /* 内核映像边界（由引导加载程序设置） */
    uint64_t kernel_start;
    uint64_t kernel_end;

    /* 保留供未来使用 */
    uint64_t reserved[8];
} bootinfo_t;

/* Bootinfo魔数值 */
#define BOOTINFO_MAGIC 0x534F584F  /* "SOXO" = SpiritFox OS */

#endif /* BOOTINFO_H */
