#include "kernel.h"
/* 每 CPU 的 GDT/TSS/IDT 与上下文切换：EFER、CR0/CR4 特性位只作用于当前
 * CPU；IDT 一次构建、所有 CPU 共享；TSS.rsp0 在任务切换时更新为私有内核栈。
 * 8259 PIC 与 PIT 时钟仍由 BSP 提供，AP 只接收 LAPIC IPI。 */
typedef struct __attribute__((packed)){uint16_t lo,selector;uint8_t ist,attr;uint16_t mid;uint32_t hi,zero;} Gate;
typedef struct __attribute__((packed)){uint16_t limit;uint64_t base;} Descriptor;
typedef struct __attribute__((packed)){uint32_t reserved;uint64_t rsp0,rsp1,rsp2,reserved2,ist[7],reserved3;uint16_t reserved4,iomap;} Tss;
static uint64_t gdt[SF_MAX_CPUS][7];
static Tss tss[SF_MAX_CPUS];
static Gate idt[256];
static uint8_t fault_stack[SF_MAX_CPUS][16384] __attribute__((aligned(16)));
static uint8_t bsp_stack[16384] __attribute__((aligned(16)));
/* 28-byte protected-mode FLDENV image: CW, SW, TW|FOP, zero pointers. */
static const uint32_t empty_fpenv[7]={0x037f,0xffff,0,0,0,0,0};
static uint8_t input[256];static unsigned input_head,input_count,input_overflow;
/* 串口控制台 IRQ 处理：轮询读入字符进环形缓冲，缓冲满置溢出标记。 */
void console_irq(void){while(inb(0x3fd)&1){uint8_t c=inb(0x3f8);if(input_count<sizeof(input)){input[(input_head+input_count)%sizeof(input)]=c;input_count++;}else input_overflow=1;}outb(0x20,0x20);}
/* 读取一个输入字符；溢出时报告 SF_EVENT_OVERFLOW 并清空缓冲。 */
long console_read(void){if(input_overflow){input_overflow=0;input_head=input_count=0;return SF_EVENT_OVERFLOW;}if(!input_count)return SF_WOULD_BLOCK;uint8_t c=input[input_head];input_head=(input_head+1)%sizeof(input);input_count--;return c;}
extern void load_gdt(Descriptor*);extern void *isr_table[];
void serial(const char*s){while(*s){if(*s=='\n'){while(!(inb(0x3fd)&32)){}outb(0x3f8,'\r');}while(!(inb(0x3fd)&32)){}outb(0x3f8,*s++);}}
void hex(uint64_t v){char b[19]="0x0000000000000000";for(int i=0;i<16;i++)b[17-i]="0123456789abcdef"[(v>>(i*4))&15];serial(b);}
/* 内核致命错误：输出后关中断并 hlt 自旋，等待调试器/复位。 */
void panic(const char*s){serial("[PANIC] ");serial(s);serial("\n");__asm__ volatile("cli");for(;;)__asm__ volatile("hlt");}
/* 加载共享 IDT；arch_cpu_init 与 AP 启动路径都经此入口。 */
void arch_load_idt(void){Descriptor id={sizeof(idt)-1,(uint64_t)idt};__asm__ volatile("lidt %0"::"m"(id));}
/* 单 CPU 初始化：每 CPU GDT/TSS、GS_BASE 自定位、EFER.NXE/LME 与 CR0/CR4
 * 特性位。GDT 必须位于恒等映射内核低端内存。 */
void arch_cpu_init(unsigned index){
 if(index>=SF_MAX_CPUS)index=0;
 uint64_t base=(uint64_t)&tss[index],limit=sizeof(Tss)-1;
 gdt[index][0]=0;
 gdt[index][1]=0x00af9a000000ffff;gdt[index][2]=0x00cf92000000ffff;
 gdt[index][3]=0x00affa000000ffff;gdt[index][4]=0x00cff2000000ffff;
 gdt[index][5]=(limit&0xffff)|((base&0xffffff)<<16)|(0x89ull<<40)|((limit&0xf0000)<<32)|((base&0xff000000)<<32);
 gdt[index][6]=base>>32;
 tss[index].iomap=sizeof(Tss);tss[index].ist[0]=(uint64_t)(fault_stack[index]+sizeof(fault_stack[index]));
 if(!tss[index].rsp0)tss[index].rsp0=cpus[index].boot_stack?cpus[index].boot_stack:(uint64_t)(bsp_stack+sizeof(bsp_stack));
 Descriptor gd={sizeof(gdt[index])-1,(uint64_t)gdt[index]};load_gdt(&gd);
 /* load_gdt 清空 GS 选择子，因此 GS_BASE 必须在它之后写入。 */
 uint64_t gs=(uint64_t)&cpus[index];
 __asm__ volatile("wrmsr"::"a"((uint32_t)gs),"d"((uint32_t)(gs>>32)),"c"(0xc0000101));
 uint64_t efer;uint32_t lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080));efer=((((uint64_t)hi<<32)|lo)|(1ull<<11))&~(1ull<<14);__asm__ volatile("wrmsr"::"a"((uint32_t)efer),"d"((uint32_t)(efer>>32)),"c"(0xc0000080));
 /* Eager x87/MMX/SSE context only. Never inherit firmware lazy/XSAVE policy. */
 uint32_t eax,ebx,ecx,edx;__asm__ volatile("cpuid":"=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(1),"c"(0));
 if((edx&0x07000001u)!=0x07000001u)panic("x87/FXSR/SSE2 required");
 uint64_t cr0,cr4;__asm__ volatile("mov %%cr0,%0":"=r"(cr0));cr0=(cr0|((1ull<<16)|(1ull<<5)|(1ull<<1)))&~((1ull<<2)|(1ull<<3));__asm__ volatile("mov %0,%%cr0"::"r"(cr0):"memory");
 /* SMEP/SMAP stay off when the CPU does not advertise them. Kernel RAM
  * accesses use the identity map, so SMAP never needs a user-window STAC. */
 uint32_t max_leaf,ebx7=0;__asm__ volatile("cpuid":"=a"(max_leaf),"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(0),"c"(0));
 if(max_leaf>=7)__asm__ volatile("cpuid":"=a"(eax),"=b"(ebx7),"=c"(ecx),"=d"(edx):"a"(7),"c"(0));
 __asm__ volatile("mov %%cr4,%0":"=r"(cr4));
 cr4=(cr4|(1ull<<9)|(1ull<<10)|((ebx7&(1u<<7))?(1ull<<20):0ull)|((ebx7&(1u<<20))?(1ull<<21):0ull))&~(1ull<<18);
 __asm__ volatile("mov %0,%%cr4"::"r"(cr4):"memory");
 if(index==0&&!(ebx7&(1u<<7)))serial("[arch] SMEP unavailable\n");
 if(index==0&&!(ebx7&(1u<<20)))serial("[arch] SMAP unavailable\n");
 arch_load_idt();
}
/* BSP 初始化：构建共享 IDT、装载 CPU 0 的 GDT/TSS、配置 8259 重映射与
 * PIT 100Hz 时钟。 */
void arch_init(void){
 for(unsigned i=0;i<256;i++){uint64_t a=(uint64_t)isr_table[i];idt[i]=(Gate){a&65535,8,i==8?1:0,i==128?0xee:0x8e,(a>>16)&65535,a>>32,0};}
 arch_cpu_init(0);
 /* PIC, PIT channel 0: 100Hz; all other IRQs remain masked. */
 outb(0x20,0x11);outb(0xa0,0x11);outb(0x21,32);outb(0xa1,40);outb(0x21,4);outb(0xa1,2);outb(0x21,1);outb(0xa1,1);outb(0x21,0xee);outb(0xa1,0xff);outb(0x3f9,1);
 outb(0x43,0x36);outb(0x40,11932&255);outb(0x40,11932>>8);
}
/* 保存当前任务的浮点状态（FXSAVE 到任务私有区域）。 */
void arch_save(Task*t){__asm__ volatile("fxsave64 %0":"=m"(t->fxstate)::"memory");}
/* 切换到目标任务：更新本 CPU TSS.rsp0、切换 CR3 地址空间、恢复浮点状态
 * （先 fninit+fldenv 清除挂起 x87 异常与遗留指针元数据，再 FXRSTOR）。 */
void arch_task(Task*t){
 tss[cpu_index()].rsp0=(uint64_t)(t->kstack+sizeof(t->kstack));__asm__ volatile("mov %0,%%cr3"::"r"(t->cr3):"memory");
 __asm__ volatile("fninit; fldenv %1; fxrstor64 %0"::"m"(t->fxstate),"m"(empty_fpenv):"memory");
}
