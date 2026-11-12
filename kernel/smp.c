#include "kernel.h"
#include "iommu.h"
/* SMP 启动与 LAPIC/IPI：MADT 收集启用 LAPIC，AP 经 0x8000 处 16→32→64 位
 * trampoline 进入 ap_entry_c，随后通过全局内核锁参与调度。BSP 仍由 8259
 * PIC + PIT 驱动，AP 只在收到 LAPIC IPI 或内核唤醒时调度。
 * 每 CPU 通过 GS_BASE 指向自己的 Cpu 记录，cpu_index() 不依赖共享全局。 */
#define LAPIC_BASE 0xfee00000ull
#define LAPIC_ID 0x20
#define LAPIC_TPR 0x80
#define LAPIC_EOI 0xb0
#define LAPIC_SVR 0xf0
#define LAPIC_ICR_LOW 0x300
#define LAPIC_ICR_HIGH 0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_THERMAL 0x330
#define LAPIC_LVT_PERF 0x340
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_ESR 0x280
/* 避开 8259 重映射后的 0x20..0x2F：PIC 级联/从片中断与 IPI 不能共用向量。 */
#define IPI_PREEMPT 0x70
#define IPI_TLB 0x71
#define LAPIC_SPURIOUS 255
#define TRAMPOLINE 0x8000
Cpu cpus[SF_MAX_CPUS];
static uint8_t ap_stacks[SF_MAX_CPUS][16384] __attribute__((aligned(16)));
void ap_entry_c(void) __attribute__((noreturn));
typedef struct {uint64_t physical,virtual_address,pages,attribute;uint32_t type,reserved;} BootMemory;
/* AP 引导映像：SIPI 总是从 0x8000 页首开始执行，因此 +0x00 先跳到代码，
 * 数据区固定放在 +0x10 PML4、+0x14 入口、+0x1C 栈顶、+0x24 BSP GDTR。
 * 0x8000 起 16 位实模式 → 32 位保护模式 → 64 位长模式；本地 GDT 只用于进入
 * 32 位阶段，长模式远跳前改用 BSP GDT，使 CS=0x08 与后续每 CPU GDT 一致。 */
static const uint8_t trampoline[219]={
 0xeb,0x4e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x17,0x00,
 0x38,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
 0xff,0xff,0x00,0x00,0x00,0x9a,0xcf,0x00,0xff,0xff,0x00,0x00,0x00,0x92,0xcf,0x00,
 0xfa,0x31,0xc0,0x8e,0xd8,0x8e,0xc0,0xe4,0x92,0x0c,0x02,0xe6,0x92,0x0f,0x01,0x16,
 0x2e,0x80,0x0f,0x20,0xc0,0x66,0x83,0xc8,0x01,0x0f,0x22,0xc0,0xea,0x71,0x80,0x08,
 0x00,0x66,0xb8,0x10,0x00,0x8e,0xd8,0x8e,0xc0,0x8e,0xd0,0x8e,0xe0,0x8e,0xe8,0xa1,
 0x10,0x80,0x00,0x00,0x0f,0x22,0xd8,0x0f,0x20,0xe0,0x83,0xc8,0x20,0x0f,0x22,0xe0,
 0xb9,0x80,0x00,0x00,0xc0,0x0f,0x32,0x0d,0x00,0x09,0x00,0x00,0x0f,0x30,0x0f,0x01,
 0x15,0x24,0x80,0x00,0x00,0x0f,0x20,0xc0,0x0d,0x00,0x00,0x00,0x80,0x0f,0x22,0xc0,
 0xea,0xb7,0x80,0x00,0x00,0x08,0x00,0x66,0xb8,0x10,0x00,0x8e,0xd8,0x8e,0xc0,0x8e,
 0xd0,0x8e,0xe0,0x8e,0xe8,0x48,0x8b,0x24,0x25,0x1c,0x80,0x00,0x00,0x48,0x8b,0x04,
 0x25,0x14,0x80,0x00,0x00,0xff,0xd0,0xfa,0xf4,0xeb,0xfc};


typedef struct __attribute__((packed)){uint16_t limit;uint64_t base;} Gdtr;
static inline uint32_t lapic_read(uint32_t reg){return *(volatile uint32_t*)(LAPIC_BASE+reg);}
static inline void lapic_write(uint32_t reg,uint32_t value){*(volatile uint32_t*)(LAPIC_BASE+reg)=value;}
/* APIC ID 来自 MMIO LAPIC ID 寄存器；同一套 xAPIC 寄存器也用于发 IPI。
 * IA32_APIC_BASE[31:24] 属于基址而非 ID，不能用于识别 CPU。 */
static uint32_t lapic_id(void){return(lapic_read(LAPIC_ID)>>24)&0xff;}
static void lapic_base_enable(void){uint32_t lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0x1b));lo=(lo&0xfffff000u)|(1u<<11);hi=0;__asm__ volatile("wrmsr"::"a"(lo),"d"(hi),"c"(0x1b));}
static void write_gs(Cpu*c){uint64_t g=(uint64_t)c;__asm__ volatile("wrmsr"::"a"((uint32_t)g),"d"((uint32_t)(g>>32)),"c"(0xc0000101));}
/* BSP 运行在固件的页表下，虚拟 0x8000 未必直映物理低内存；借用 AP 的
 * 静态内核地址空间（低 2 GiB identity）写入再恢复，保证 AP 在实模式取到映像。 */
static void low_write(uint64_t at,const void*data,size_t n){
 uint64_t old;__asm__ volatile("mov %%cr3,%0":"=r"(old));
 __asm__ volatile("mov %0,%%cr3"::"r"(space_ap_cr3()):"memory");
 memcpy((void*)at,data,n);
 __asm__ volatile("mov %0,%%cr3"::"r"(old):"memory");
}
/* 当前 CPU 索引：GS_BASE 指向 Cpu 记录；早期未设置时按 BSP 处理。 */
unsigned cpu_index(void){uint32_t lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000101));uint64_t g=((uint64_t)hi<<32)|lo;return g?(unsigned)((Cpu*)g)->index:0;}
unsigned smp_online(void){unsigned n=0;for(unsigned i=0;i<SF_MAX_CPUS;i++)if(__atomic_load_n(&cpus[i].online,__ATOMIC_ACQUIRE))n++;return n;}
/* 只启用固定交付、物理目标的 IPI；ICR 忙时自旋等待。 */
static void lapic_send(uint32_t id,uint32_t low){if(!(lapic_read(LAPIC_SVR)&0x100))return;while(lapic_read(LAPIC_ICR_LOW)&(1u<<12))__asm__ volatile("pause");lapic_write(LAPIC_ICR_HIGH,id<<24);lapic_write(LAPIC_ICR_LOW,low);}
static void lapic_broadcast(uint32_t vector){uint32_t low=vector|(3u<<18);while(lapic_read(LAPIC_ICR_LOW)&(1u<<12))__asm__ volatile("pause");lapic_write(LAPIC_ICR_LOW,low);}
/* 初始化本核 LAPIC：软件使能、清 TPR/ESR、屏蔽定时器与非 PIC 通道；
 * BSP 的 LINT0 保持 ExtINT 以接收 8259，AP 屏蔽全部本地输入。 */
static void lapic_init(unsigned index){
 lapic_base_enable();lapic_write(LAPIC_TPR,0);lapic_write(LAPIC_SVR,0x100|LAPIC_SPURIOUS);
 lapic_write(LAPIC_LVT_TIMER,1u<<16);lapic_write(LAPIC_LVT_THERMAL,1u<<16);lapic_write(LAPIC_LVT_PERF,1u<<16);
 lapic_write(LAPIC_LVT_LINT1,1u<<16);lapic_write(LAPIC_LVT_ERROR,1u<<16);
 lapic_write(LAPIC_LVT_LINT0,index==0?0x700:(1u<<16));
 lapic_write(LAPIC_ESR,0);lapic_write(LAPIC_ESR,0);
}
void apic_eoi(void){lapic_write(LAPIC_EOI,0);}
void smp_preempt_all(void){if(smp_online()>1)lapic_broadcast(IPI_PREEMPT);}
void smp_tlb_flush_all(void){if(smp_online()>1)lapic_broadcast(IPI_TLB);}
/* 真实时间延时：INIT 与 SIPI 之间必须有足够间隔，否则还在运行的 AP
 *（固件停放循环）会丢弃 SIPI。TSC 在 QEMU/真实硬件上随宿主时间前进。 */
static uint64_t tsc(void){uint32_t lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));return((uint64_t)hi<<32)|lo;}
static void delay_us(uint64_t us){uint64_t end=tsc()+us*1000;while(tsc()<end)__asm__ volatile("pause");}
static void decimal(unsigned v){char b[12];unsigned n=0;do{b[n++]='0'+(v%10);v/=10;}while(v);for(unsigned i=0;i<n/2;i++){char t=b[i];b[i]=b[n-1-i];b[n-1-i]=t;}b[n]=0;serial(b);}
static int firmware_read(void*c,uint64_t address,void*dst,size_t bytes){
 if(address>=0x100000000ull||bytes>0x100000000ull-address)return 1;
 SfBootInfo*b=c;BootMemory*map=(void*)b->memory_map;
 for(uint64_t i=0;i<b->memory_map_entry_count;i++){BootMemory*m=&map[i];if((m->type!=9&&m->type!=10)||m->pages>UINT64_MAX/4096)continue;uint64_t n=m->pages*4096;if(address>=m->physical&&address-m->physical<n&&bytes<=n-(address-m->physical)){memcpy(dst,(void*)(uintptr_t)address,bytes);return 0;}}
 return 1;
}
static void start_ap(unsigned index,const Gdtr*gd){
 cpus[index].boot_stack=(uint64_t)(ap_stacks[index]+sizeof(ap_stacks[index]));
 uint8_t image[sizeof(trampoline)];memcpy(image,trampoline,sizeof(image));
 *(uint32_t*)(image+0x10)=(uint32_t)space_ap_cr3();*(uint64_t*)(image+0x14)=(uint64_t)ap_entry_c;
 *(uint64_t*)(image+0x1C)=cpus[index].boot_stack;memcpy(image+0x24,gd,sizeof(*gd));
 low_write(TRAMPOLINE,image,sizeof(image));
 /* INIT assert（交付模式 5+电平置位）；随后用 0x0500 撤销 INIT，
  * 不能再写 0x4400——那是交付模式 4 的 NMI，会在 AP 启动后立刻打崩它。 */
 lapic_send(cpus[index].apic_id,0x4500);delay_us(50000);
 lapic_send(cpus[index].apic_id,0x0500);delay_us(20000);
 lapic_send(cpus[index].apic_id,0x608);delay_us(20000);
 lapic_send(cpus[index].apic_id,0x608);delay_us(20000);
 /* 有界等待 AP 上线：成功路径通常在数十毫秒内完成。 */
 uint64_t end=tsc()+1000000000ull;
 while(tsc()<end){if(__atomic_load_n(&cpus[index].online,__ATOMIC_ACQUIRE))return;__asm__ volatile("pause");}
}
/* SMP 启动：BSP 先建立自己的 Cpu 记录与 LAPIC，然后按 MADT 串行拉起 AP。
 * 每个 AP 各用独立启动栈；SIPI 前补丁 trampoline 的栈顶槽。 */
void smp_init(SfBootInfo*b){
 cpus[0].index=0;cpus[0].bsp=1;cpus[0].online=1;
 space_apic_init();write_gs(&cpus[0]);lapic_init(0);cpus[0].apic_id=lapic_id();
 serial("SMP: bsp apic=");hex(cpus[0].apic_id);serial("\n");
 SfMadtInfo info;unsigned count=1;
 if(!sf_acpi_madt(firmware_read,b,b->acpi_rsdp,&info)&&info.count){
  for(unsigned i=0;i<info.count&&count<SF_MAX_CPUS;i++)if(info.apic_ids[i]!=cpus[0].apic_id){cpus[count].index=count;cpus[count].apic_id=info.apic_ids[i];cpus[count].online=0;cpus[count].bsp=0;count++;}
 }
 Gdtr gd;__asm__ volatile("sgdt %0":"=m"(gd));
 for(unsigned i=1;i<count;i++){start_ap(i,&gd);if(cpus[i].online){serial("SMP: AP ");decimal(i);serial(" online\n");}else{serial("SMP: AP ");decimal(i);serial(" timeout\n");}}
 serial("SMP: cpus=");decimal(smp_online());serial("\n");
}
/* AP 入口：trampoline 已切到本核栈；建立每 CPU GDT/TSS/IDT 与 LAPIC 后
 * 加入全局调度循环，永不返回。 */
void ap_entry_c(void) __attribute__((noreturn));
void ap_entry_c(void){
 lapic_base_enable();uint32_t id=lapic_id();unsigned index=SF_MAX_CPUS;
 for(unsigned i=1;i<SF_MAX_CPUS;i++)if(cpus[i].boot_stack&&cpus[i].apic_id==id){index=i;break;}
 if(index==SF_MAX_CPUS){__asm__ volatile("cli");for(;;)__asm__ volatile("hlt");}
 arch_cpu_init(index);lapic_init(index);
 __atomic_store_n(&cpus[index].online,1,__ATOMIC_RELEASE);
 for(;;){kernel_lock_acquire();Frame*f=schedule();kernel_lock_release();arch_enter(f);}
}
