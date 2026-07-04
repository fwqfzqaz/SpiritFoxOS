#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include "boot.h"

#define PAGE_SIZE 4096

void memory_init(BootInfo* info, uintptr_t kernel_end);
void* alloc_page(void);
void* alloc_pages(size_t count);
void free_page(void* addr);
uint64_t pmm_total_pages(void);
uint64_t pmm_used_pages(void);

#endif
