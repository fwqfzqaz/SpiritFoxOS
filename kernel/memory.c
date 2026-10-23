#include "kernel.h"
#define POOL_PAGES 4096
static uint8_t pool[POOL_PAGES][4096] __attribute__((aligned(4096)));
static uint32_t owner[POOL_PAGES];
static uint64_t kernel_pd[512] __attribute__((aligned(4096)));
static uint64_t mmio_pd[512] __attribute__((aligned(4096))),mmio_pt[512] __attribute__((aligned(4096)));
static int mmio_ready;
static uint64_t device_pt[512] __attribute__((aligned(4096)));
static uint64_t*boot_mmio_pd;
int space_mmio_init(uint64_t address){
 if(address!=0xfed90000ull)return SF_DENIED;uint64_t cr3;__asm__ volatile("mov %%cr3,%0":"=r"(cr3));
 uint64_t*p4=(void*)cr3,*p3=(void*)(p4[0]&0x000ffffffffff000ull),*p2=(void*)(p3[3]&0x000ffffffffff000ull);
 if(!(p3[3]&1)||!(p2[(address>>21)&511]&128))return SF_BAD_FORMAT;
 p2[(address>>21)&511]=(address&~0x1fffffull)|0x9b|NX;__asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");
 mmio_pd[(address>>21)&511]=(uint64_t)mmio_pt|3;mmio_pt[(address>>12)&511]=address|0x1b|NX;boot_mmio_pd=p2;mmio_ready=1;return 0;
}
int space_device_mmio(uint64_t physical){
 if(!mmio_ready||(physical&4095)||physical>=0x1000000000000ull)return SF_DENIED;
 device_pt[0]=physical|0x1b|NX;mmio_pd[511]=(uint64_t)device_pt|3;boot_mmio_pd[511]=(uint64_t)device_pt|3;
 uint64_t cr3;__asm__ volatile("mov %%cr3,%0":"=r"(cr3));__asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");return 0;
}
void *page_alloc(uint32_t pid){for(unsigned i=0;i<POOL_PAGES;i++)if(!owner[i]){owner[i]=pid;memset(pool[i],0,4096);return pool[i];}return 0;}
void pages_free(uint32_t pid){for(unsigned i=0;i<POOL_PAGES;i++)if(owner[i]==pid){memset(pool[i],0,4096);owner[i]=0;}}
uint64_t space_new(uint32_t pid){
 if(!kernel_pd[0])for(unsigned i=0;i<512;i++)kernel_pd[i]=i*0x200000ull|0x83;
 uint64_t*p4=page_alloc(pid),*p3=page_alloc(pid);if(!p4||!p3)return 0;
 p4[0]=(uint64_t)p3|7;p3[0]=(uint64_t)kernel_pd|3;if(mmio_ready)p3[3]=(uint64_t)mmio_pd|3;return(uint64_t)p4;
}
static uint64_t *pte(Task*t,uint64_t v,int create){
 if(v<SF_BASE||v>=0x80000000ull)return 0;uint64_t *p4=(void*)t->cr3,*p3=(void*)(p4[0]&0x000ffffffffff000ull);unsigned a=v>>30,b=(v>>21)&511,c=(v>>12)&511;
 if(!(p3[a]&1)){if(!create)return 0;void*p=page_alloc(t->pid);if(!p)return 0;p3[a]=(uint64_t)p|7;}
 uint64_t*p2=(void*)(p3[a]&0x000ffffffffff000ull);
 if(!(p2[b]&1)){if(!create)return 0;void*p=page_alloc(t->pid);if(!p)return 0;p2[b]=(uint64_t)p|7;}
 uint64_t*p1=(void*)(p2[b]&0x000ffffffffff000ull);return&p1[c];
}
int space_map(Task*t,uint64_t v,uint64_t physical,uint32_t flags){
 if((v|physical)&4095||!(flags&1)||(flags&6)==6)return SF_DENIED;
 uint64_t*p=pte(t,v,1);if(!p)return SF_NO_SPACE;if(*p&1)return SF_INVALID;
 *p=physical|5|((flags&2)?2:0)|((flags&4)?0:NX);return SF_OK;
}
int space_map_mmio(Task*t,uint64_t v,uint64_t physical){int r=space_map(t,v,physical,3);if(!r)*pte(t,v,0)|=0x18;return r;}
int user_range(Task*t,uint64_t v,uint64_t n,int write){
 if(v<SF_BASE||v>=0x80000000ull||n>0x80000000ull-v)return 0;if(!n)return 1;
 uint64_t end=(v+n-1)&~4095ull;for(uint64_t a=v&~4095ull;;a+=4096){uint64_t*p=pte(t,a,0);if(!p||!(*p&1)||!(*p&4)||(write&&!(*p&2)))return 0;if(a==end)return 1;}
}
void *user_phys(Task*t,uint64_t v){uint64_t*p=pte(t,v,0);return p&&(*p&1)?(void*)((*p&0x000ffffffffff000ull)+(v&4095)):0;}
int copy_in(Task*t,void*dst,uint64_t src,size_t n){if(!user_range(t,src,n,0))return SF_DENIED;uint8_t*d=dst;while(n){size_t k=4096-(src&4095);if(k>n)k=n;memcpy(d,user_phys(t,src),k);d+=k;src+=k;n-=k;}return 0;}
int copy_out(Task*t,uint64_t dst,const void*src,size_t n){if(!user_range(t,dst,n,1))return SF_DENIED;const uint8_t*s=src;while(n){size_t k=4096-(dst&4095);if(k>n)k=n;memcpy(user_phys(t,dst),s,k);s+=k;dst+=k;n-=k;}return 0;}
