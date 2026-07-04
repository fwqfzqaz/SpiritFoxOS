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

extern tss_t tss;

void gdt_init(void);
void gdt_set_tss(uint64_t rsp0);

#endif
