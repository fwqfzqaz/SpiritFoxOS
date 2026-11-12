#include "kernel.h"
/* 物理页与用户地址空间管理：静态私有页池、页表构建、映射与用户指针校验。
 * 所有页池页分配时清零、归还时清零，杜绝跨任务数据残留。 */
#define POOL_PAGES SF_PAGE_LIMIT_MAX
/* PTE 第 9 位：设备 MMIO 标记（user_range 据此拒绝把设备寄存器当 RAM 缓冲）。 */
#define PTE_MMIO (1ull<<9)
static uint8_t pool[POOL_PAGES][4096] __attribute__((aligned(4096)));
static uint32_t owner[POOL_PAGES];
static uint64_t kernel_pd[512] __attribute__((aligned(4096)));
/* 内核侧 MMIO 虚拟窗口（0xfed90000=VT-d 寄存器基址；511 项=按设备动态窗口）。 */
static uint64_t mmio_pd[512] __attribute__((aligned(4096))),mmio_pt[512] __attribute__((aligned(4096)));
/* IOAPIC 与 VT-d 寄存器同处 0xFEC00000 起的 2 MiB 窗口；单独一页页表
 * 避免与共享 mmio_pt 的 0 号项（LAPIC）冲突。 */
static uint64_t ioapic_pt[512] __attribute__((aligned(4096)));
static int mmio_ready,device_ready;
static uint64_t device_pt[512] __attribute__((aligned(4096)));
static uint64_t*boot_mmio_pd;
static uint64_t ap_p4[512] __attribute__((aligned(4096))),ap_p3[512] __attribute__((aligned(4096)));
/* 固件 framebuffer 所在 1 GiB 区域的共享 PD：内核在任务 CR3 下处理
 * SYS_LOG 时会写屏幕，若任务地址空间缺少该区域就会触发页错误。 */
static uint64_t fb_pd[512] __attribute__((aligned(4096)));
static unsigned fb_group;
static int fb_ready;
int space_fb_init(uint64_t base,uint64_t bytes){
 if(!base||!bytes||base>0x100000000ull||bytes>0x100000000ull-base)return SF_DENIED;
 unsigned group=(unsigned)(base>>30);if(group>3)return SF_DENIED;
 if(group==3){
  uint64_t first=(base>>21)&511u,last=((base+bytes-1)>>21)&511u;
  for(uint64_t i=first;i<=last;i++)mmio_pd[i]=(0xc0000000ull+i*0x200000ull)|0x9b|NX;
  mmio_ready=1;return 0;
 }
 if(!group)return 0; /* 0..1 GiB 已由 kernel_pd 直映 */
 if(group!=2)return SF_DENIED; /* 1..2 GiB 是用户窗口，不能整段占用 */
 for(unsigned i=0;i<512;i++)fb_pd[i]=0;
 uint64_t first=(base>>21)&511u,last=((base+bytes-1)>>21)&511u;
 for(uint64_t i=first;i<=last;i++)fb_pd[i]=((uint64_t)group*0x40000000ull+i*0x200000ull)|0x9b|NX;
 fb_group=group;fb_ready=1;return 0;
}
/* 把 VT-d 寄存器窗口挂进内核页表：引导页表中的 2 MiB 段改写为 RW+NX，
 * 并在 mmio_pd/mmio_pt 中建立等价的用户侧只读映射结构（供设备窗口复用）。 */
int space_mmio_init(uint64_t address){
 if(address!=0xfed90000ull)return SF_DENIED;uint64_t cr3;__asm__ volatile("mov %%cr3,%0":"=r"(cr3));
 uint64_t*p4=(void*)cr3,*p3=(void*)(p4[0]&0x000ffffffffff000ull),*p2=(void*)(p3[3]&0x000ffffffffff000ull);
 if(!(p3[3]&1)||!(p2[(address>>21)&511]&128))return SF_BAD_FORMAT;
 p2[(address>>21)&511]=(address&~0x1fffffull)|0x9b|NX;__asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");
 mmio_pd[(address>>21)&511]=(uint64_t)mmio_pt|3;mmio_pt[(address>>12)&511]=address|0x1b|NX;boot_mmio_pd=p2;mmio_ready=1;device_ready=1;return 0;
}
/* 把 0xFEE00000 LAPIC 页挂进引导页表与共享 mmio_pd；所有 CPU 与所有地址空间都可见。 */
void space_apic_init(void){
 uint64_t cr3;__asm__ volatile("mov %%cr3,%0":"=r"(cr3));
 uint64_t*p4=(void*)cr3,*p3=(void*)(p4[0]&0x000ffffffffff000ull);
 if(!(p3[3]&1))return;uint64_t*p2=(void*)(p3[3]&0x000ffffffffff000ull);
 p2[(0xfee00000ull>>21)&511]=0xfee00000ull|0x9b|NX;
 mmio_pd[(0xfee00000ull>>21)&511]=(uint64_t)mmio_pt|3;mmio_pt[0]=0xfee00000ull|0x1b|NX;
 boot_mmio_pd=p2;mmio_ready=1;__asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");
}
/* 把 0xFEC00000 IOAPIC 页挂进引导页表与共享 mmio_pd。该 2 MiB 段同时
 * 容纳 0xFED90000 VT-d 寄存器，因此新建一页页表并保留 VT-d 映射。 */
int space_ioapic_init(void){
 uint64_t cr3;__asm__ volatile("mov %%cr3,%0":"=r"(cr3));
 uint64_t*p4=(void*)cr3,*p3=(void*)(p4[0]&0x000ffffffffff000ull);
 if(!(p3[3]&1))return SF_BAD_FORMAT;
 uint64_t*p2=(void*)(p3[3]&0x000ffffffffff000ull);unsigned index=(0xfec00000ull>>21)&511;
 if(!(p2[index]&128))return SF_BAD_FORMAT;
 p2[index]=0xfec00000ull|0x9b|NX;
 ioapic_pt[0]=0xfec00000ull|0x1b|NX;
 if(mmio_ready)ioapic_pt[(0xfed90000ull>>12)&511]=0xfed90000ull|0x1b|NX;
 mmio_pd[index]=(uint64_t)ioapic_pt|3;boot_mmio_pd=p2;mmio_ready=1;
 __asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");return 0;
}
/* AP 使用的静态内核地址空间：低 2 GiB 直映内核，3..4 GiB 窗口映射 LAPIC/VT-d。 */
uint64_t space_ap_cr3(void){
 if(!kernel_pd[0])for(unsigned i=0;i<512;i++)kernel_pd[i]=i*0x200000ull|0x83;
 ap_p4[0]=(uint64_t)ap_p3|7;ap_p3[0]=(uint64_t)kernel_pd|3;if(mmio_ready)ap_p3[3]=(uint64_t)mmio_pd|3;
 if(fb_ready)ap_p3[fb_group]=(uint64_t)fb_pd|3;
 return(uint64_t)ap_p4;
}
/* 为设备授予 MMIO 窗口：把物理页挂到 device_pt 并映射进所有地址空间的
 * 511 号 PD 项（引导页表与各任务页表共享该页表页），刷新 TLB 生效。 */
int space_device_mmio(uint64_t physical){
 if(!device_ready||(physical&4095)||physical>=0x1000000000000ull)return SF_DENIED;
 device_pt[0]=physical|0x1b|NX;mmio_pd[511]=(uint64_t)device_pt|3;boot_mmio_pd[511]=(uint64_t)device_pt|3;
 uint64_t cr3;__asm__ volatile("mov %%cr3,%0":"=r"(cr3));__asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");return 0;
}
/* 校验 PID 归属：页配额记在进程主线程上。 */
static Task *page_owner(uint32_t pid){
 if(!pid||pid>INT32_MAX)return 0;
 for(unsigned i=0;i<SF_MAX_THREADS;i++){Task*t=&tasks[i];if(t->main&&t->pid==pid&&(t->state==T_EMPTY||t->state==T_READY||t->state==T_BLOCKED))return t;}
 return 0;
}
/* 分配一页：计入任务页配额（超限返回 0），清零后返回内核虚拟地址。 */
void *page_alloc(uint32_t pid){
 Task*t=page_owner(pid);if(!t||t->page_count>=t->page_limit)return 0;
 for(unsigned i=0;i<POOL_PAGES;i++)if(!owner[i]){owner[i]=pid;t->page_count++;memset(pool[i],0,4096);return pool[i];}return 0;
}
/* 归还单个已分配但尚未映射的页（线程创建失败路径）。 */
void page_free_phys(uint32_t pid,uint64_t physical){
 if(!pid||physical<(uint64_t)pool||physical>=(uint64_t)pool+sizeof(pool))return;
 unsigned i=(unsigned)((physical-(uint64_t)pool)/4096);if(owner[i]!=pid)return;
 memset(pool[i],0,4096);owner[i]=0;Task*m=page_owner(pid);if(m&&m->page_count)m->page_count--;
}
/* 归还任务全部页：逐页清零防泄漏；同时把该 PID 任务页计数归零。 */
void pages_free(uint32_t pid){
 if(!pid||pid>INT32_MAX)return;
 for(unsigned i=0;i<POOL_PAGES;i++)if(owner[i]==pid){memset(pool[i],0,4096);owner[i]=0;}
 /* task_dead marks DEAD before freeing; construction rollback is EMPTY. */
 for(unsigned i=0;i<SF_MAX_THREADS;i++)if(tasks[i].pid==pid)tasks[i].page_count=0;
}
/* 新建地址空间：P4/P3 两级新建，P3 低 2 GiB 复用内核直映 PD（共享内核镜像），
 * MMIO 就绪后共享 mmio_pd。返回 P4 物理地址作为 CR3。 */
uint64_t space_new(uint32_t pid){
 if(!kernel_pd[0])for(unsigned i=0;i<512;i++)kernel_pd[i]=i*0x200000ull|0x83;
 uint64_t*p4=page_alloc(pid),*p3=page_alloc(pid);if(!p4||!p3)return 0;
 p4[0]=(uint64_t)p3|7;p3[0]=(uint64_t)kernel_pd|3;if(mmio_ready)p3[3]=(uint64_t)mmio_pd|3;
 if(fb_ready)p3[fb_group]=(uint64_t)fb_pd|3;return(uint64_t)p4;
}
/* 查找/按需创建三级页表项路径；v 必须位于用户窗口 [SF_BASE,0x80000000)。 */
static uint64_t *pte(Task*t,uint64_t v,int create){
 if(v<SF_BASE||v>=0x80000000ull)return 0;uint64_t *p4=(void*)t->cr3,*p3=(void*)(p4[0]&0x000ffffffffff000ull);unsigned a=v>>30,b=(v>>21)&511,c=(v>>12)&511;
 if(!(p3[a]&1)){if(!create)return 0;void*p=page_alloc(t->pid);if(!p)return 0;p3[a]=(uint64_t)p|7;}
 uint64_t*p2=(void*)(p3[a]&0x000ffffffffff000ull);
 if(!(p2[b]&1)){if(!create)return 0;void*p=page_alloc(t->pid);if(!p)return 0;p2[b]=(uint64_t)p|7;}
 uint64_t*p1=(void*)(p2[b]&0x000ffffffffff000ull);return&p1[c];
}
/* 建立用户映射：flags bit0=存在、bit1=可写、bit2=可执行（不可执行时置 NX）。
 * 拒绝非对齐地址、不存在位缺失与"可写且可执行"（W^X 约束）。 */
int space_map(Task*t,uint64_t v,uint64_t physical,uint32_t flags){
 if((v|physical)&4095||!(flags&1)||(flags&6)==6)return SF_DENIED;
 uint64_t*p=pte(t,v,1);if(!p)return SF_NO_SPACE;if(*p&1)return SF_INVALID;
 *p=physical|5|((flags&2)?2:0)|((flags&4)?0:NX);return SF_OK;
}
/* 设备 MMIO 映射：RW+NX 并附加 PTE_MMIO/Cache 禁用位（第 4-5 位）。 */
int space_map_mmio(Task*t,uint64_t v,uint64_t physical){int r=space_map(t,v,physical,3);if(!r)*pte(t,v,0)|=0x18|PTE_MMIO;return r;}
/* 显示内存映射：仅允许固定基址，逐页 RW+NX 并禁用缓存。
 * Framebuffer 不是设备寄存器，因此不置 PTE_MMIO（允许按普通 RAM 写）。 */
int space_map_fb(Task*t,uint64_t v,uint64_t physical,uint64_t bytes){
 if(v!=SF_FB||(physical&4095)||!bytes||bytes>SF_FB_BYTES||physical>0x000ffffffffff000ull-bytes)return SF_INVALID;
 for(uint64_t off=0;off<bytes;off+=4096){
  uint64_t*p=pte(t,v+off,0);
  if(p&&(*p&1)){ /* 已映射且指向同一物理页则只补缓存位，支持重试。 */
   if((*p&0x000ffffffffff000ull)!=physical+off)return SF_INVALID;
   *p|=0x18;continue;
  }
  int r=space_map(t,v+off,physical+off,3);if(r)return r;
  *pte(t,v+off,0)|=0x18;
 }
 return SF_OK;
}
/* 用户区间校验：整段逐页检查存在/用户/可写位，且拒绝 MMIO 页——
 * copy 系列经内核直映解引用物理地址，设备寄存器不是 RAM 缓冲。 */
int user_range(Task*t,uint64_t v,uint64_t n,int write){
 if(v<SF_BASE||v>=0x80000000ull||n>0x80000000ull-v)return 0;if(!n)return 1;
 /* Copy helpers dereference RAM through the kernel identity map. Device
  * registers are not RAM buffers, even when the caller can access them. */
 uint64_t end=(v+n-1)&~4095ull;for(uint64_t a=v&~4095ull;;a+=4096){uint64_t*p=pte(t,a,0);if(!p||!(*p&1)||!(*p&4)||(*p&PTE_MMIO)||(write&&!(*p&2)))return 0;if(a==end)return 1;}
}
void *user_phys(Task*t,uint64_t v){uint64_t*p=pte(t,v,0);return p&&(*p&1)?(void*)((*p&0x000ffffffffff000ull)+(v&4095)):0;}
/* 入口点校验：必须落在用户窗口内、存在、用户可访问、非 NX。 */
int user_exec(Task*t,uint64_t v){uint64_t*p=pte(t,v,0);return p&&(*p&1)&&(*p&4)&&!(*p&NX);}
/* 解除一段用户映射并归还页池物理页；线程栈退出使用。 */
int space_unmap_range(Task*t,uint64_t v,uint64_t bytes){
 if((v&4095)||(bytes&4095)||!bytes||v<SF_BASE||v>=0x80000000ull||bytes>0x80000000ull-v)return SF_INVALID;
 Task*m=task_main(t);int freed=0;
 for(uint64_t a=v;a<v+bytes;a+=4096){uint64_t*p=pte(t,a,0);if(!p||!(*p&1))continue;
  uint64_t physical=*p&0x000ffffffffff000ull;
  if(physical>=(uint64_t)pool&&physical<(uint64_t)pool+sizeof(pool)){
   unsigned i=(unsigned)((physical-(uint64_t)pool)/4096);
   if(owner[i]==t->pid){memset(pool[i],0,4096);owner[i]=0;if(m->page_count)m->page_count--;freed=1;}
  }
  *p=0;
#ifndef SF_HOST_TEST
  __asm__ volatile("invlpg (%0)"::"r"(a):"memory");
#endif
 }
 if(freed&&smp_online()>1)smp_tlb_flush_all();
 return SF_OK;
}
/* 匿名映射：只在堆窗口内建立 RW+NX 页；addr==0 时自底寻找连续空洞。
 * 分配失败按已建立的部分回滚，保证要么整段可用、要么完全不留痕迹。 */
long space_map_anon(Task*t,uint64_t addr,uint64_t bytes){
 if((addr&4095)||(bytes&4095)||!bytes||bytes>SF_HEAP_BYTES)return SF_INVALID;
 if(!addr){
  for(uint64_t a=SF_HEAP;a<=SF_HEAP+SF_HEAP_BYTES-bytes;a+=4096){
   int free=1;
   for(uint64_t b=a;b<a+bytes;b+=4096){uint64_t*p=pte(t,b,0);if(p&&(*p&1)){free=0;break;}}
   if(free){addr=a;break;}
  }
  if(!addr)return SF_NO_SPACE;
 }else if(addr<SF_HEAP||addr>SF_HEAP+SF_HEAP_BYTES-bytes)return SF_INVALID;
 for(uint64_t off=0;off<bytes;off+=4096){
  uint64_t*p=pte(t,addr+off,0);
  if(p&&(*p&1)){space_unmap_range(t,addr,off);return SF_INVALID;}
 }
 for(uint64_t off=0;off<bytes;off+=4096){
  void*page=page_alloc(t->pid);
  if(!page){space_unmap_range(t,addr,off);return SF_NO_SPACE;}
  int r=space_map(t,addr+off,(uint64_t)page,3);
  if(r){page_free_phys(t->pid,(uint64_t)page);space_unmap_range(t,addr,off);return r;}
 }
 return(long)addr;
}
/* 跨页复制：先整段 user_range 校验，再按页边界分段经内核直映搬运。 */
int copy_in(Task*t,void*dst,uint64_t src,size_t n){if(!user_range(t,src,n,0))return SF_DENIED;uint8_t*d=dst;while(n){size_t k=4096-(src&4095);if(k>n)k=n;memcpy(d,user_phys(t,src),k);d+=k;src+=k;n-=k;}return 0;}
int copy_out(Task*t,uint64_t dst,const void*src,size_t n){if(!user_range(t,dst,n,1))return SF_DENIED;const uint8_t*s=src;while(n){size_t k=4096-(dst&4095);if(k>n)k=n;memcpy(user_phys(t,dst),s,k);s+=k;dst+=k;n-=k;}return 0;}
