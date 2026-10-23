#ifndef SF_IOMMU_H
#define SF_IOMMU_H
#include "sf.h"
#define SF_VTD_MMIO 0xfed90000ull
#define SF_DMA_IOVA 0x01000000ull
typedef int (*SfPhysRead)(void*,uint64_t,void*,size_t);
typedef struct {uint64_t base;uint8_t covered[256];} SfDmarInfo;
int sf_acpi_dmar(SfPhysRead,void*,uint64_t,SfDmarInfo*);
typedef struct {
 uint64_t root[512],context[512],level3[512],level2[512],level1[512];
} SfVtdTables;
int sf_vtd_tables(SfVtdTables*,uint64_t,uint32_t,uint64_t);
#endif
