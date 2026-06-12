/* SpiritFoxOS - 全局描述符表接口
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
#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* GDT选择子 */
#define GDT_NULL_SEL    0x00
#define GDT_CODE_SEL    0x08
#define GDT_DATA_SEL    0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS_SEL     0x28

/* GDT条目结构（代码/数据段8字节，TSS 16字节） */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access_byte;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
} __attribute__((packed));

/* TSS描述符（16字节，跨越两个GDT槽位） */
struct tss_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid_low;
    uint8_t  access_byte;
    uint8_t  flags_limit_high;
    uint8_t  base_mid_high;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed));

/* 任务状态段 */
struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];       /* RSP0、RSP1、RSP2 */
    uint64_t reserved1;
    uint64_t ist[7];       /* IST1-IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* GDTR（全局描述符表寄存器） */
struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void gdt_init(void);
struct tss *get_tss(void);

#endif /* GDT_H */
