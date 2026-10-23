/* Hardware authorization only. Virtqueue and block protocols live in sfmod. */
#include "kernel.h"
static uint8_t dma[SF_DMA_BYTES] __attribute__((aligned(4096)));
static uint32_t device_owner;
static int quarantined;
static void outl(uint16_t port,uint32_t value){__asm__ volatile("outl %0,%1"::"a"(value),"Nd"(port));}
static uint32_t inl(uint16_t port){uint32_t v;__asm__ volatile("inl %1,%0":"=a"(v):"Nd"(port));return v;}
static uint32_t config_read(uint32_t bdf,uint32_t offset){outl(0xcf8,0x80000000u|(bdf<<8)|offset);return inl(0xcfc);}
static void config_write_command(uint32_t bdf,uint16_t command){outl(0xcf8,0x80000004u|(bdf<<8));__asm__ volatile("outw %0,%1"::"a"(command),"Nd"((uint16_t)0xcfc));}
long pci_read(uint64_t bdf,uint64_t offset){if(bdf>65535||offset>252||(offset&3))return SF_INVALID;return config_read(bdf,offset);}
int device_release(Task*t){
 if(!t->device_io)return t->device_fault?SF_DEVICE_GONE:0;
 /* Keep DMA storage pinned even after successful reset. Never recycle it as RAM. */
 outb(t->device_io+18,0);
 uint16_t command=config_read(t->device_bdf,4)&0xffff;
 config_write_command(t->device_bdf,command&~4u);
 int reset=0;for(unsigned i=0;i<10000;i++)if(inb(t->device_io+18)==0){reset=1;break;}
 if((config_read(t->device_bdf,4)&4)||!reset){quarantined=1;serial("[CORE] DMA device quarantined; pinned pages retained\n");}
 t->device_fault=quarantined;t->device_io=0;
 return quarantined?SF_DEVICE_GONE:0;
}
/* A released live driver still maps the pinned DMA arena. Only allow another
 * owner after the old address space has been destroyed, including failure paths. */
void device_forget(Task*t){if(device_owner==t->pid)device_owner=0;}
int device_grant(Task*t,uint32_t bdf_plus_one,uint32_t rights,SfBootArgs*args){
 if(!bdf_plus_one||bdf_plus_one>65536)return SF_INVALID;
 if(device_owner||quarantined)return SF_BUSY;
 uint32_t bdf=bdf_plus_one-1;if(config_read(bdf,0)!=0x10011af4u)return SF_DENIED;
 uint32_t bar=config_read(bdf,16);if(!(bar&1)||(bar&~3u)<0x1000||(bar&~3u)>0xffc0)return SF_DENIED;
 t->device_io=bar&~3u;t->device_bdf=bdf;
 if(device_release(t))return SF_DEVICE_GONE;
 t->device_io=bar&~3u;device_owner=t->pid;memset(dma,0,sizeof(dma));
 for(unsigned off=0;off<sizeof(dma);off+=4096)if(space_map(t,SF_DMA+off,(uint64_t)(dma+off),3)){device_release(t);return SF_NO_SPACE;}
 uint16_t command=config_read(bdf,4)&0xffff;config_write_command(bdf,command|5);
 if((config_read(bdf,4)&5)!=5){device_release(t);return SF_DEVICE_GONE;}
 args->dma_virtual=SF_DMA;args->dma_physical=(uint64_t)dma;args->device_io=t->device_io;args->device_bdf=bdf;args->device_rights=rights;
 return 0;
}
long device_io(Task*t,uint64_t port,uint64_t operation,uint64_t value){
 unsigned width=operation&255;int write=(operation&256)!=0;
 if(operation&~0x1ffull||(width!=1&&width!=2&&width!=4))return SF_INVALID;
 if(!t->device_io||device_owner!=t->pid||port<t->device_io||port>t->device_io+64u-width)return SF_DENIED;
 if(write){if(width==1)outb(port,value);else if(width==2)__asm__ volatile("outw %0,%1"::"a"((uint16_t)value),"Nd"((uint16_t)port));else outl(port,value);return 0;}
 if(width==1)return inb(port);if(width==2){uint16_t v;__asm__ volatile("inw %1,%0":"=a"(v):"Nd"((uint16_t)port));return v;}return inl(port);
}
