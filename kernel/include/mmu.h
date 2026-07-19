#ifndef MMU_H
#define MMU_H

#include <stdint.h>
#include <hal.h>

uint64_t *mmu_walk_page(uint64_t pml4_phys, uint64_t virt, int create);
int mmu_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t mmu_virt_to_phys(uint64_t pml4_phys, uint64_t virt);
void mmu_init(void);

#endif /* MMU_H */
