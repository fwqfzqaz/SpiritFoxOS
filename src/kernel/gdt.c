#include "gdt.h"
#include "../include/io.h"

/* GDT: 5 entries (8 bytes each) + 1 TSS descriptor (16 bytes) = 56 bytes total */
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
    low |= ((uint64_t)0x89 << 40);  /* Present, 64-bit TSS */
    low |= ((uint64_t)((limit >> 16) & 0x0F) << 48);
    low |= ((uint64_t)((base >> 24) & 0xFF) << 56);
    high = (uint64_t)(base >> 32);
    gdt[idx] = low;
    gdt[idx + 1] = high;
}

/* Separate function for CS reload */
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
    /* Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* 64-bit kernel code segment */
    gdt_set_entry(1, 0, 0, 0x9A, 0x0A);

    /* Kernel data segment */
    gdt_set_entry(2, 0, 0, 0x92, 0x0C);

    /* 64-bit user code segment */
    gdt_set_entry(3, 0, 0, 0xFA, 0x0A);

    /* User data segment */
    gdt_set_entry(4, 0, 0, 0xF2, 0x0C);

    /* TSS descriptor at index 5 (selector 0x28) */
    for (int i = 0; i < (int)sizeof(kernel_tss); i++) {
        ((uint8_t *)&kernel_tss)[i] = 0;
    }
    kernel_tss.iomap_base = sizeof(struct tss);
    gdt_set_tss(5, (uint64_t)&kernel_tss, sizeof(struct tss) - 1);

    /* Load GDTR */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)gdt;
    __asm__ volatile ("lgdt %0" : : "m"(gdt_ptr));

    /* Reload data segment selectors */
    __asm__ volatile (
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        : : : "ax", "memory"
    );

    /* Reload CS */
    reload_cs();

    /* Load TSS */
    uint16_t tss_sel = 0x28;
    __asm__ volatile ("ltr %0" : : "r"(tss_sel));
}

struct tss *get_tss(void) {
    return &kernel_tss;
}
