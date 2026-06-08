#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include "pmm.h"

/* Page table entry flags */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_HUGE      (1ULL << 7)
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Page table structure */
typedef struct {
    uint64_t entries[512];
} page_table_t __attribute__((aligned(4096)));

void vmm_init(void);
int vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_unmap_page(page_table_t *pml4, uint64_t virt);
uint64_t vmm_get_physical(page_table_t *pml4, uint64_t virt);
page_table_t *vmm_create_pml4(void);
void vmm_destroy_pml4(page_table_t *pml4);

/* Physical to virtual conversion (identity mapped in first 1GB) */
static inline uint64_t phys_to_virt(uint64_t phys) {
    return phys; /* Identity mapping for first 1GB */
}

static inline uint64_t virt_to_phys(uint64_t virt) {
    return virt; /* Identity mapping for first 1GB */
}

page_table_t *vmm_get_kernel_pml4(void);

#endif /* VMM_H */
