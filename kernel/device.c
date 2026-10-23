/* Hardware authorization only. Virtqueue and block protocols live in sfmod. */
#include "kernel.h"
static uint8_t dma[SF_DMA_BYTES] __attribute__((aligned(4096)));
static uint32_t device_owner;
static int quarantined;
static uint8_t guard[4096] __attribute__((aligned(4096)));
static SfDmaStatus dma_status;
static void outl(uint16_t port,uint32_t value){__asm__ volatile("outl %0,%1"::"a"(value),"Nd"(port));}
static uint32_t inl(uint16_t port){uint32_t v;__asm__ volatile("inl %1,%0":"=a"(v):"Nd"(port));return v;}
static uint32_t config_read(uint32_t bdf,uint32_t offset){outl(0xcf8,0x80000000u|(bdf<<8)|offset);return inl(0xcfc);}
static void config_write_command(uint32_t bdf,uint16_t command){outl(0xcf8,0x80000004u|(bdf<<8));__asm__ volatile("outw %0,%1"::"a"(command),"Nd"((uint16_t)0xcfc));}
long pci_read(uint64_t bdf,uint64_t offset){if(bdf>65535||offset>252||(offset&3))return SF_INVALID;return config_read(bdf,offset);}
void device_init(SfBootInfo*b){
 memset(guard,0xa5,sizeof(guard));
 /* Firmware no longer owns any bus-zero DMA engines after ExitBootServices. */
 for(unsigned bdf=0;bdf<256;bdf++)if(config_read(bdf,0)!=UINT32_MAX){uint16_t command=config_read(bdf,4);if(command&4)config_write_command(bdf,command&~4u);}
 iommu_init(b);
}
int device_status(SfDmaStatus*out){
 dma_status.size=sizeof(dma_status);dma_status.mode=iommu_mode();dma_status.owner=device_owner;dma_status.probe_address=(uint64_t)guard;dma_status.guard_intact=1;dma_status.active_mapping=iommu_active();
 for(unsigned i=0;i<sizeof(guard);i++)if(guard[i]!=0xa5)dma_status.guard_intact=0;*out=dma_status;return 0;
}
int device_release(Task*t){
 if(!t->device_io)return t->device_fault?SF_DEVICE_GONE:0;
 /* Keep DMA storage pinned even after successful reset. Never recycle it as RAM. */
 if(t->device_modern)*(volatile uint8_t*)0xffe00014ull=0;else outb(t->device_io+18,0);
 uint16_t command=config_read(t->device_bdf,4)&0xffff;
 config_write_command(t->device_bdf,command&~4u);
 int reset=0;for(unsigned i=0;i<10000;i++)if((t->device_modern?*(volatile uint8_t*)0xffe00014ull:inb(t->device_io+18))==0){reset=1;break;}
 int invalidation=iommu_revoke(t->device_bdf);
 if((config_read(t->device_bdf,4)&4)||!reset||invalidation){quarantined=1;serial("[CORE] DMA device quarantined; pinned pages retained\n");}
 t->device_fault=quarantined;t->device_io=0;
 return quarantined?SF_DEVICE_GONE:0;
}
/* A released live driver still maps the pinned DMA arena. Only allow another
 * owner after the old address space has been destroyed, including failure paths. */
void device_forget(Task*t){if(device_owner==t->pid)device_owner=0;}
void device_poll(void){
 uint32_t source,reason;uint64_t address;
 for(unsigned fault=0;fault<64&&iommu_fault(&source,&reason,&address);fault++){
  dma_status.faults++;dma_status.source=source;dma_status.reason=reason;dma_status.last_address=address;
  serial("[IOMMU] blocked DMA source=");hex(source);serial(" address=");hex(address);serial(" reason=");hex(reason);serial("\n");
  for(unsigned i=0;i<SF_MAX_TASKS;i++){Task*t=&tasks[i];if(t->pid==device_owner&&t->device_bdf==source&&t->state!=T_DEAD){device_release(t);quarantined=1;t->device_fault=1;task_dead(t,SF_DEVICE_GONE);serial("[IOMMU] offending driver terminated and quarantined\n");break;}}
 }
}
int device_grant(Task*t,uint32_t bdf_plus_one,uint32_t rights,SfBootArgs*args){
 if(!bdf_plus_one||bdf_plus_one>65536)return SF_INVALID;
 if(device_owner||quarantined)return SF_BUSY;if(iommu_mode()==2)return SF_DEVICE_GONE;
 uint32_t bdf=bdf_plus_one-1,identity=config_read(bdf,0);int modern=identity==0x10421af4u;if(identity!=0x10011af4u&&!modern)return SF_DENIED;
 if(iommu_mode()==1&&!modern)return SF_DENIED; /* Legacy virtio bypasses platform DMA. */
 uint32_t bar=config_read(bdf,modern?32:16);uint64_t physical=0;
 if(modern){
  if(iommu_mode()!=1||(bar&7)!=4)return SF_DENIED;physical=((uint64_t)config_read(bdf,36)<<32)|(bar&~15u);if((physical&16383)||physical<0xc0000000ull||physical>=0x1000000000000ull-16384)return SF_DENIED;
  /* The validated QEMU modern profile places common/device/notify in BAR4. */
  unsigned cap=config_read(bdf,0x34)&255,found=0;for(unsigned step=0;cap&&step<48;step++){if(cap<0x40||cap>0xfc||(cap&3))return SF_BAD_FORMAT;uint32_t h=config_read(bdf,cap);if((h&255)==9){unsigned kind=h>>24,length=(h>>16)&255;if(length<16||cap>240)return SF_BAD_FORMAT;uint32_t id=config_read(bdf,cap+4)&255,off=config_read(bdf,cap+8),n=config_read(bdf,cap+12);if((kind==1||kind==2||kind==4)&&id!=4)return SF_DENIED;if(kind==1&&off==0&&n>=56)found|=1;if(kind==2&&off==0x3000&&n>=2&&length>=20&&cap<=236&&config_read(bdf,cap+16)==4)found|=2;if(kind==4&&off==0x2000&&n>=8)found|=4;}cap=(h>>8)&255;}
  if(found!=7||cap||space_device_mmio(physical))return SF_DENIED;t->device_io=SF_DEVICE_MMIO;t->device_modern=1;
 }else{if(!(bar&1)||(bar&~3u)<0x1000||(bar&~3u)>0xffc0)return SF_DENIED;t->device_io=bar&~3u;}
 t->device_bdf=bdf;
 if(device_release(t))return SF_DEVICE_GONE;
 t->device_io=modern?SF_DEVICE_MMIO:bar&~3u;device_owner=t->pid;memset(dma,0,sizeof(dma));
 if(modern)for(unsigned off=0;off<16384;off+=4096)if(space_map_mmio(t,SF_DEVICE_MMIO+off,physical+off)){device_release(t);return SF_NO_SPACE;}
 for(unsigned off=0;off<sizeof(dma);off+=4096)if(space_map(t,SF_DMA+off,(uint64_t)(dma+off),3)){device_release(t);return SF_NO_SPACE;}
 uint64_t bus_address;int mapped=iommu_grant(bdf,(uint64_t)dma,&bus_address);if(mapped){device_release(t);return mapped;}
 uint16_t command=config_read(bdf,4)&0xffff,enable=modern?6:5;config_write_command(bdf,command|enable);
 if((config_read(bdf,4)&enable)!=enable){device_release(t);return SF_DEVICE_GONE;}
 args->dma_virtual=SF_DMA;args->dma_physical=bus_address;args->device_io=t->device_io;args->device_bdf=bdf;args->device_rights=rights|(iommu_mode()==1?SF_DEVICE_ISOLATED:0)|(modern?SF_DEVICE_MODERN:0);
 return 0;
}
long device_io(Task*t,uint64_t port,uint64_t operation,uint64_t value){
 unsigned width=operation&255;int write=(operation&256)!=0;
 if(operation&~0x1ffull||(width!=1&&width!=2&&width!=4))return SF_INVALID;
 if(t->device_modern||!t->device_io||device_owner!=t->pid||port<t->device_io||port>t->device_io+64u-width)return SF_DENIED;
 if(write){if(width==1)outb(port,value);else if(width==2)__asm__ volatile("outw %0,%1"::"a"((uint16_t)value),"Nd"((uint16_t)port));else outl(port,value);return 0;}
 if(width==1)return inb(port);if(width==2){uint16_t v;__asm__ volatile("inw %1,%0":"=a"(v):"Nd"((uint16_t)port));return v;}return inl(port);
}
