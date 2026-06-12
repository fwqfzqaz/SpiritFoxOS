/* SpiritFoxOS - 全局描述符表
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
#include "gdt.h"
#include "../include/io.h"

/* GDT：5个条目（每个8字节）+ 1个TSS描述符（16字节）= 共56字节 */
static uint64_t gdt[7] __attribute__((aligned(16)));
static struct gdtr gdt_ptr;
static struct tss kernel_tss __attribute__((aligned(16)));

static void gdt_set_entry(int idx, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t flags) {
    uint64_t entry = 0;
    entry |= (uint64_t)(limit & 0xFFFF);
    entry |= ((uint64_t)(base & 0xFFFF) << 16);
    entry |= ((uint64_t)((base >> 16) & 0xFF) << 32);
    entry |= ((uint64_t)access << 40);
    entry |= ((uint64_t)(((flags & 0x0F) << 4) | ((limit >> 16) & 0x0F)) << 48);
    entry |= ((uint64_t)((base >> 24) & 0xFF) << 56);
    gdt[idx] = entry;
}

static void gdt_set_tss(int idx, uint64_t base, uint32_t limit) {
    uint64_t low = 0, high = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= ((uint64_t)(base & 0xFFFF) << 16);
    low |= ((uint64_t)((base >> 16) & 0xFF) << 32);
    low |= ((uint64_t)0x89 << 40);  /* 存在，64位TSS */
    low |= ((uint64_t)((limit >> 16) & 0x0F) << 48);
    low |= ((uint64_t)((base >> 24) & 0xFF) << 56);
    high = (uint64_t)(base >> 32);
    gdt[idx] = low;
    gdt[idx + 1] = high;
}

/* 单独的函数用于重新加载CS */
__attribute__((noinline)) static void reload_cs(void) {
    __asm__ volatile (
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        : : : "rax", "memory"
    );
}

void gdt_init(void) {
    /* 空描述符 */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* 64位内核代码段 */
    gdt_set_entry(1, 0, 0, 0x9A, 0x0A);

    /* 内核数据段 */
    gdt_set_entry(2, 0, 0, 0x92, 0x0C);

    /* 64位用户代码段 */
    gdt_set_entry(3, 0, 0, 0xFA, 0x0A);

    /* 用户数据段 */
    gdt_set_entry(4, 0, 0, 0xF2, 0x0C);

    /* TSS描述符在索引5处（选择子0x28） */
    for (int i = 0; i < (int)sizeof(kernel_tss); i++) {
        ((uint8_t *)&kernel_tss)[i] = 0;
    }
    kernel_tss.iomap_base = sizeof(struct tss);
    gdt_set_tss(5, (uint64_t)&kernel_tss, sizeof(struct tss) - 1);

    /* 加载GDTR */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)gdt;
    __asm__ volatile ("lgdt %0" : : "m"(gdt_ptr));

    /* 重新加载数据段选择子 */
    __asm__ volatile (
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        : : : "ax", "memory"
    );

    /* 重新加载CS */
    reload_cs();

    /* 加载TSS */
    uint16_t tss_sel = 0x28;
    __asm__ volatile ("ltr %0" : : "r"(tss_sel));
}

struct tss *get_tss(void) {
    return &kernel_tss;
}
