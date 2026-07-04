#include "gdt.h"

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

static GDTEntry gdt[GDT_ENTRIES];
static GDTPtr   gdt_ptr;

tss_t tss;

static void gdt_set_gate(uint8_t num, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran)
{
    gdt[num].base_low    = base & 0xFFFF;
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;

    gdt[num].limit_low   = limit & 0xFFFF;
    gdt[num].granularity = (limit >> 16) & 0x0F;

    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access = access;
}

static void gdt_flush(uint64_t gdt_ptr_addr)
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
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        :
        : "r"(gdt_ptr_addr)
        : "rax", "memory"
    );
}

void gdt_set_tss(uint64_t rsp0)
{
    tss.rsp0 = rsp0;

    uint32_t base  = (uint64_t)&tss & 0xFFFFFFFF;
    uint32_t limit = sizeof(tss_t) - 1;

    /* TSS low descriptor (entry 5): 64-bit TSS, present */
    gdt_set_gate(5, base, limit, 0x89, 0x00);

    /* TSS high descriptor (entry 6): upper 32 bits of base address */
    uint32_t base_high = (uint64_t)&tss >> 32;
    gdt[6].limit_low   = base_high & 0xFFFF;
    gdt[6].base_low    = (base_high >> 16) & 0xFFFF;
    gdt[6].base_middle = 0;
    gdt[6].access      = 0;
    gdt[6].granularity = 0;
    gdt[6].base_high   = 0;
}

void gdt_init(void)
{
    gdt_ptr.limit = sizeof(GDTEntry) * GDT_ENTRIES - 1;
    gdt_ptr.base  = (uint64_t)&gdt;

    /* Null descriptor */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* Kernel code segment: base=0, limit=4GB, 64-bit code, ring 0
     * gran=0xA0: G=1(4KB), D=0, L=1(64-bit), AVL=0 */
    gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0xA0);

    /* Kernel data segment: base=0, limit=4GB, read/write, ring 0
     * gran=0xC0: G=1(4KB), D/B=1(32-bit), L=0, AVL=0 */
    gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0xC0);

    /* User code segment: base=0, limit=4GB, 64-bit code, ring 3
     * gran=0xA0: G=1(4KB), D=0, L=1(64-bit), AVL=0 */
    gdt_set_gate(3, 0, 0xFFFFF, 0xFA, 0xA0);

    /* User data segment: base=0, limit=4GB, read/write, ring 3
     * gran=0xC0: G=1(4KB), D/B=1(32-bit), L=0, AVL=0 */
    gdt_set_gate(4, 0, 0xFFFFF, 0xF2, 0xC0);

    /* Initialize TSS */
    for (int i = 0; i < (int)sizeof(tss_t); i++)
        ((uint8_t *)&tss)[i] = 0;
    tss.iomap_base = sizeof(tss_t);

    /* Set up TSS descriptors (entries 5 and 6) */
    gdt_set_tss(0);

    gdt_flush((uint64_t)&gdt_ptr);

    /* Load the task register */
    __asm__ volatile ("ltr %0" : : "r"((uint16_t)GDT_TSS_SEL));
}
