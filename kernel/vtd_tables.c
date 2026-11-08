#include "iommu.h"
/* Legacy root/context, domain 1, three-level 39-bit second-stage tables.
 * The root/context table is shared; each device slot owns its own level tree
 * so two devices can be granted concurrently without aliasing one another. */
static int vtd_build(SfVtdTables*t,uint64_t address,uint32_t bdf,uint64_t physical,unsigned slot,int clear){
 if((address|physical)&4095||address>=0x40000000ull||sizeof(*t)>0x40000000ull-address||physical>=0x40000000ull||SF_DMA_BYTES>0x40000000ull-physical||bdf>=256||slot>=SF_DMA_IOVA_SLOTS)return SF_INVALID;
 uint64_t l3=address+offsetof(SfVtdTables,level3)+slot*sizeof(t->level3[0]);
 uint64_t l2=address+offsetof(SfVtdTables,level2)+slot*sizeof(t->level2[0]);
 uint64_t l1=address+offsetof(SfVtdTables,level1)+slot*sizeof(t->level1[0]);
 uint64_t iova=SF_DMA_IOVA+(uint64_t)slot*SF_DMA_IOVA_STRIDE;
 if(clear)memset(t,0,sizeof(*t));
 t->root[0]=(address+offsetof(SfVtdTables,context))|1;
 t->context[bdf*2]=l3|1;t->context[bdf*2+1]=0x101;
 t->level3[slot][0]=l2|3;
 t->level2[slot][iova>>21]=l1|3;
 for(unsigned i=0;i<SF_DMA_BYTES/4096;i++)t->level1[slot][i]=(physical+i*4096)|3;
 return 0;
}
int sf_vtd_tables(SfVtdTables*t,uint64_t address,uint32_t bdf,uint64_t physical){
 return vtd_build(t,address,bdf,physical,0,1);
}
int sf_vtd_tables_add(SfVtdTables*t,uint64_t address,uint32_t bdf,uint64_t physical,unsigned slot){
 return vtd_build(t,address,bdf,physical,slot,0);
}
