#include "iommu.h"
/* Legacy root/context, domain 1, three-level 39-bit second-stage tables. */
int sf_vtd_tables(SfVtdTables*t,uint64_t address,uint32_t bdf,uint64_t physical){
 if((address|physical)&4095||address>=0x40000000ull||sizeof(*t)>0x40000000ull-address||physical>=0x40000000ull||SF_DMA_BYTES>0x40000000ull-physical||bdf>=256)return SF_INVALID;
 memset(t,0,sizeof(*t));t->root[0]=(address+offsetof(SfVtdTables,context))|1;
 t->context[bdf*2]=(address+offsetof(SfVtdTables,level3))|1;t->context[bdf*2+1]=0x101;
 t->level3[0]=(address+offsetof(SfVtdTables,level2))|3;
 t->level2[SF_DMA_IOVA>>21]=(address+offsetof(SfVtdTables,level1))|3;
 for(unsigned i=0;i<SF_DMA_BYTES/4096;i++)t->level1[i]=(physical+i*4096)|3;return 0;
}
