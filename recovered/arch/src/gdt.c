#include "gdt.h"
#include "smp.h"
#include <string.h>

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) GDTEntry;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) GDTPtr;

/* ========================================================================
 * Per-CPU GDT 全局数组
 * ======================================================================== */

cpu_gdt_t cpu_gdts[SMP_MAX_CPUS];

/* 全局 tss 变量向后兼容 — 指向 cpu_gdts[0].tss */
tss_t tss;

/* ========================================================================
 * 内部辅助函数
 * ======================================================================== */

static void gdt_set_gate_entry(uint8_t *entries, uint8_t num,
                                uint32_t base, uint32_t limit,
                                uint8_t access, uint8_t gran)
{
    GDTEntry *gdt = (GDTEntry *)entries;
    gdt[num].base_low    = base & 0xFFFF;
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;

    gdt[num].limit_low   = limit & 0xFFFF;
    gdt[num].granularity = (limit >> 16) & 0x0F;

    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access = access;
}

static void gdt_flush_cpu(uint64_t gdt_ptr_addr)
{
    __asm__ volatile (
        "lgdt (%0)\n\t"
        "pushq $0x08\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        /* 不设置 GS — GS 基地址由 MSR 管理，per-CPU 数据依赖它 */
        "mov %%ax, %%ss\n\t"
        :
        : "r"(gdt_ptr_addr)
        : "rax", "memory"
    );
}

/* 设置指定 CPU 的 TSS 描述符（条目 5 和 6） */
static void gdt_write_tss_descriptor(int cpu_index)
{
    cpu_gdt_t *cg = &cpu_gdts[cpu_index];
    tss_t *t = &cg->tss;

    uint32_t base  = (uint64_t)t & 0xFFFFFFFF;
    uint32_t limit = sizeof(tss_t) - 1;

    /* TSS 低位描述符（条目 5）：64 位 TSS，存在 */
    gdt_set_gate_entry(cg->entries, 5, base, limit, 0x89, 0x00);

    /* TSS 高位描述符（条目 6）：基地址的高 32 位 */
    GDTEntry *gdt = (GDTEntry *)cg->entries;
    uint32_t base_high = (uint64_t)t >> 32;
    gdt[6].limit_low   = base_high & 0xFFFF;
    gdt[6].base_low    = (base_high >> 16) & 0xFFFF;
    gdt[6].base_middle = 0;
    gdt[6].access      = 0;
    gdt[6].granularity = 0;
    gdt[6].base_high   = 0;
}

/* ========================================================================
 * Per-CPU GDT API
 * ======================================================================== */

void gdt_init_cpu(int cpu_index)
{
    cpu_gdt_t *cg = &cpu_gdts[cpu_index];

    /* 设置 GDTR 参数 */
    cg->limit = sizeof(GDTEntry) * GDT_ENTRIES - 1;
    cg->base  = (uint64_t)cg->entries;

    /* 空描述符 */
    gdt_set_gate_entry(cg->entries, 0, 0, 0, 0, 0);

    /* 内核代码段：基地址=0，界限=4GB，64 位代码，特权级 0
     * gran=0xA0：G=1(4KB)，D=0，L=1(64位)，AVL=0 */
    gdt_set_gate_entry(cg->entries, 1, 0, 0xFFFFF, 0x9A, 0xA0);

    /* 内核数据段：基地址=0，界限=4GB，读/写，特权级 0
     * gran=0xC0：G=1(4KB)，D/B=1(32位)，L=0，AVL=0 */
    gdt_set_gate_entry(cg->entries, 2, 0, 0xFFFFF, 0x92, 0xC0);

    /* 用户代码段：基地址=0，界限=4GB，64 位代码，特权级 3
     * gran=0xA0：G=1(4KB)，D=0，L=1(64位)，AVL=0 */
    gdt_set_gate_entry(cg->entries, 3, 0, 0xFFFFF, 0xFA, 0xA0);

    /* 用户数据段：基地址=0，界限=4GB，读/写，特权级 3
     * gran=0xC0：G=1(4KB)，D/B=1(32位)，L=0，AVL=0 */
    gdt_set_gate_entry(cg->entries, 4, 0, 0xFFFFF, 0xF2, 0xC0);

    /* 初始化 TSS */
    memset(&cg->tss, 0, sizeof(tss_t));
    cg->tss.iomap_base = sizeof(tss_t);

    /* 设置 TSS 描述符（条目 5 和 6） */
    gdt_write_tss_descriptor(cpu_index);

    /* BSP 的 TSS 作为全局 tss 的向后兼容 */
    if (cpu_index == 0) {
        memcpy(&tss, &cg->tss, sizeof(tss_t));
    }
}

void gdt_load_cpu(int cpu_index)
{
    cpu_gdt_t *cg = &cpu_gdts[cpu_index];

    /* 构造 GDTPtr 并加载 */
    GDTPtr ptr;
    ptr.limit = cg->limit;
    ptr.base  = cg->base;

    gdt_flush_cpu((uint64_t)&ptr);

    /* 加载任务寄存器 */
    __asm__ volatile ("ltr %0" : : "r"((uint16_t)GDT_TSS_SEL));
}

void gdt_set_tss_rsp0(int cpu_index, uint64_t rsp0)
{
    cpu_gdt_t *cg = &cpu_gdts[cpu_index];
    cg->tss.rsp0 = rsp0;

    /* 更新 TSS 描述符中的基地址（TSS 地址不变，但描述符需同步） */
    gdt_write_tss_descriptor(cpu_index);

    /* 向后兼容：同步到全局 tss */
    if (cpu_index == 0) {
        tss.rsp0 = rsp0;
    }
}

void gdt_init(void)
{
    /* 初始化 BSP 的 GDT（CPU 0） */
    gdt_init_cpu(0);
    gdt_load_cpu(0);
}
