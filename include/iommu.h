#ifndef SF_IOMMU_H
#define SF_IOMMU_H
#include "sf.h"
#define SF_VTD_MMIO 0xfed90000ull
#define SF_DMA_IOVA 0x01000000ull
/* Each concurrently granted device owns a separate 2 MiB IOVA window and a
 * separate second-stage level tree; the shared root/context table carries both. */
#define SF_DMA_IOVA_STRIDE 0x200000ull
#define SF_DMA_IOVA_SLOTS 2u
typedef int (*SfPhysRead)(void*,uint64_t,void*,size_t);
typedef struct {uint64_t base;uint8_t covered[256];} SfDmarInfo;
int sf_acpi_dmar(SfPhysRead,void*,uint64_t,SfDmarInfo*);
typedef struct {uint64_t lapic_base;uint32_t apic_ids[SF_MAX_CPUS];uint32_t count;} SfMadtInfo;
int sf_acpi_madt(SfPhysRead,void*,uint64_t,SfMadtInfo*);
typedef struct {
 uint64_t root[512],context[512];
 uint64_t level3[SF_DMA_IOVA_SLOTS][512],level2[SF_DMA_IOVA_SLOTS][512],level1[SF_DMA_IOVA_SLOTS][512];
} SfVtdTables;
int sf_vtd_tables(SfVtdTables*,uint64_t,uint32_t,uint64_t);
/* Add a second device to the shared root/context table without disturbing slot 0. */
int sf_vtd_tables_add(SfVtdTables*,uint64_t,uint32_t,uint64_t,unsigned);
#endif
