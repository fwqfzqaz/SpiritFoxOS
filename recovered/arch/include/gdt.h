#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define GDT_ENTRIES 7

#define GDT_NULL_SELECTOR     0x00
#define GDT_KERNEL_CODE_SEL   0x08
#define GDT_KERNEL_DATA_SEL   0x10
#define GDT_USER_CODE_SEL     0x18
#define GDT_USER_DATA_SEL     0x20
#define GDT_TSS_SEL           0x28

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint32_t reserved2;
    uint32_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

/* ========================================================================
 * Per-CPU GDT 结构
 * ======================================================================== */

typedef struct {
    uint8_t  entries[GDT_ENTRIES * 8];  /* GDT 条目（原始字节） */
    tss_t    tss;                       /* 本 CPU 的 TSS */
    uint16_t limit;                     /* GDTR 界限 */
    uint64_t base;                      /* GDTR 基地址 */
} cpu_gdt_t;

/* 全局 GDT 数组（索引 = CPU logical index） */
extern cpu_gdt_t cpu_gdts[];

/* 保留全局 tss 变量用于向后兼容（指向 cpu_gdts[0].tss） */
extern tss_t tss;

/* ========================================================================
 * GDT API
 * ======================================================================== */

/* BSP 初始化（内部调用 gdt_init_cpu(0)） */
void gdt_init(void);

/* 初始化指定 CPU 的 GDT 条目和 TSS */
void gdt_init_cpu(int cpu_index);

/* 加载指定 CPU 的 GDT 并装载 TR */
void gdt_load_cpu(int cpu_index);

/* 设置指定 CPU TSS 的 rsp0 并更新 GDT 描述符 */
void gdt_set_tss_rsp0(int cpu_index, uint64_t rsp0);

/* 向后兼容宏 */
#define gdt_set_tss(rsp0) gdt_set_tss_rsp0(0, rsp0)

#endif