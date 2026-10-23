/* Q35 VT-d legacy mode: one segment, one bus, one fixed DMA grant. */
#include "kernel.h"
#include "iommu.h"
static SfVtdTables tables __attribute__((aligned(4096)));
static volatile uint8_t*registers;
static unsigned mode,iotlb_offset,fault_offset,fault_records;
static uint64_t capability;
static int coherent;
static SfDmarInfo topology;
static uint32_t active_bdf=UINT32_MAX;
static uint32_t r32(unsigned off){return *(volatile uint32_t*)(registers+off);}
static uint64_t r64(unsigned off){return *(volatile uint64_t*)(registers+off);}
static void w32(unsigned off,uint32_t n){*(volatile uint32_t*)(registers+off)=n;}
static void w64(unsigned off,uint64_t n){*(volatile uint64_t*)(registers+off)=n;}
static void barrier(void){__atomic_thread_fence(__ATOMIC_SEQ_CST);}
static void table_sync(void){barrier();if(!coherent)__asm__ volatile("wbinvd":::"memory");barrier();}
static int wait32(unsigned off,uint32_t mask,uint32_t value){for(unsigned i=0;i<1000000;i++){if((r32(off)&mask)==value)return 0;__asm__ volatile("pause");}return SF_TIMEOUT;}
static int wait64(unsigned off,uint64_t mask){for(unsigned i=0;i<1000000;i++){if(!(r64(off)&mask))return 0;__asm__ volatile("pause");}return SF_TIMEOUT;}
typedef struct {uint64_t physical,virtual_address,pages,attribute;uint32_t type,reserved;} FirmwareMemory;
static int firmware_read(void*context,uint64_t address,void*out,size_t bytes){
 SfBootInfo*b=context;if(address<4096||address>=0x40000000ull||bytes>0x40000000ull-address)return SF_BAD_FORMAT;
 FirmwareMemory*map=(void*)b->memory_map;
 for(uint64_t i=0;i<b->memory_map_entry_count;i++){FirmwareMemory*m=&map[i];if((m->type!=9&&m->type!=10)||m->pages>UINT64_MAX/4096)continue;uint64_t size=m->pages*4096;if(address>=m->physical&&address-m->physical<=size&&bytes<=size-(address-m->physical)){memcpy(out,(void*)address,bytes);return 0;}}
 return SF_BAD_FORMAT;
}
static int invalidate(void){
 table_sync();w64(0x28,(1ull<<63)|(1ull<<61));if(wait64(0x28,1ull<<63)||((r64(0x28)>>59)&3)!=1)goto fail;
 uint64_t flags=(1ull<<63)|(1ull<<60);if(capability&(1ull<<54))flags|=1ull<<48;if(capability&(1ull<<55))flags|=1ull<<49;
 w64(iotlb_offset,flags);if(wait64(iotlb_offset,1ull<<63)||((r64(iotlb_offset)>>57)&3)!=1)goto fail;barrier();return 0;
fail:mode=2;return SF_DEVICE_GONE;
}
void iommu_init(SfBootInfo*b){
 int r=sf_acpi_dmar(firmware_read,b,b->acpi_rsdp,&topology);uint64_t base=topology.base;
 if(r==SF_NOT_FOUND){serial("[IOMMU] absent; trusted DMA mode\n");return;}
 mode=2;if(r||space_mmio_init(base)){serial("[IOMMU] unsupported DMAR; device grants disabled code=");hex((uint64_t)(int64_t)r);serial(" rsdp=");hex(b->acpi_rsdp);serial(" base=");hex(base);serial("\n");return;}
 registers=(void*)base;capability=r64(8);uint64_t extended=r64(16);coherent=extended&1;
 iotlb_offset=((extended>>8)&1023)*16+8;fault_offset=((capability>>24)&1023)*16;fault_records=((capability>>40)&255)+1;
 /* 39-bit AGAW, registers within one MMIO page; explicit cache synchronization. */
 if(!(capability&(1ull<<9))||((capability>>16)&63)<38||iotlb_offset<0x88||iotlb_offset>4088||fault_offset<0x100||fault_offset>4096-fault_records*16||fault_records>64||r32(0x1c)&((1u<<31)|(1u<<26)|(1u<<25))){serial("[IOMMU] unsupported capabilities; device grants disabled cap=");hex(capability);serial(" ecap=");hex(extended);serial(" status=");hex(r32(0x1c));serial("\n");return;}
 memset(&tables,0,sizeof(tables));w32(0x38,1u<<31); /* Poll faults; no MSI remapping. */
 table_sync();w64(0x20,(uint64_t)&tables);barrier();w32(0x18,1u<<30);
 if(wait32(0x1c,1u<<30,1u<<30)||invalidate()){serial("[IOMMU] root/invalidation failed\n");return;}
 if(capability&(1ull<<4)){w32(0x18,1u<<27);if(wait32(0x1c,1u<<27,0))return;}
 w32(0x18,1u<<31);if(wait32(0x1c,1u<<31,1u<<31))return;
 mode=1;serial("[IOMMU] VT-d active; default deny, 39-bit tables\n");
}
unsigned iommu_mode(void){return mode;}
int iommu_grant(uint32_t bdf,uint64_t physical,uint64_t*address){
 if(!mode){*address=physical;return 0;}if(mode!=1||active_bdf!=UINT32_MAX)return SF_DEVICE_GONE;if(bdf>=256||!topology.covered[bdf])return SF_DENIED;
 if(sf_vtd_tables(&tables,(uint64_t)&tables,bdf,physical)||invalidate())return SF_DEVICE_GONE;
 active_bdf=bdf;*address=SF_DMA_IOVA;return 0;
}
int iommu_revoke(uint32_t bdf){
 if(!mode)return 0;if(mode!=1)return SF_DEVICE_GONE;if(active_bdf==UINT32_MAX)return 0;if(active_bdf!=bdf)return SF_DENIED;
 tables.context[bdf*2]=0;barrier();if(invalidate())return SF_DEVICE_GONE;memset(&tables,0,sizeof(tables));table_sync();active_bdf=UINT32_MAX;return 0;
}
int iommu_active(void){return active_bdf!=UINT32_MAX;}
int iommu_fault(uint32_t*source,uint32_t*reason,uint64_t*address){
 if(mode!=1)return 0;for(unsigned i=0;i<fault_records;i++){unsigned off=fault_offset+i*16;uint64_t high=r64(off+8);if(!(high&(1ull<<63)))continue;
  *source=high&65535;*reason=(high>>32)&255;*address=r64(off)&~4095ull;w64(off+8,1ull<<63);w32(0x34,1);return 1;}
 return 0;
}
