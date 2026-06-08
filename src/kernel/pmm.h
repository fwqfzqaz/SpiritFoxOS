#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "../include/bootinfo.h"

#define PAGE_SIZE 4096
#define BITS_PER_QWORD 64

void pmm_init(bootinfo_t *bootinfo, uint64_t kernel_start, uint64_t kernel_end);
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t addr);
uint64_t pmm_alloc_pages(uint64_t n);
uint64_t pmm_free_count(void);
uint64_t pmm_total_count(void);

#endif /* PMM_H */
