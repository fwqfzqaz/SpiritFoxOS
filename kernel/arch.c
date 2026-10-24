#include "kernel.h"
typedef struct __attribute__((packed)){uint16_t lo,selector;uint8_t ist,attr;uint16_t mid;uint32_t hi,zero;} Gate;
typedef struct __attribute__((packed)){uint16_t limit;uint64_t base;} Descriptor;
typedef struct __attribute__((packed)){uint32_t reserved;uint64_t rsp0,rsp1,rsp2,reserved2,ist[7],reserved3;uint16_t reserved4,iomap;} Tss;
static uint64_t gdt[7]={0,0x00af9a000000ffff,0x00cf92000000ffff,0x00affa000000ffff,0x00cff2000000ffff,0,0};
/* 28-byte protected-mode FLDENV image: no instruction/data pointers. */
static const uint32_t empty_fpenv[7]={0x037f,0,0xffff,0,0,0,0};
static Tss tss;static Gate idt[256];static uint8_t fault_stack[16384] __attribute__((aligned(16)));
static uint8_t input[256];static unsigned input_head,input_count,input_overflow;
void console_irq(void){while(inb(0x3fd)&1){uint8_t c=inb(0x3f8);if(input_count<sizeof(input)){input[(input_head+input_count)%sizeof(input)]=c;input_count++;}else input_overflow=1;}outb(0x20,0x20);}
long console_read(void){if(input_overflow){input_overflow=0;input_head=input_count=0;return SF_EVENT_OVERFLOW;}if(!input_count)return SF_WOULD_BLOCK;uint8_t c=input[input_head];input_head=(input_head+1)%sizeof(input);input_count--;return c;}
extern void load_gdt(Descriptor*);extern void *isr_table[];
void serial(const char*s){while(*s){if(*s=='\n'){while(!(inb(0x3fd)&32)){}outb(0x3f8,'\r');}while(!(inb(0x3fd)&32)){}outb(0x3f8,*s++);}}
void hex(uint64_t v){char b[19]="0x0000000000000000";for(int i=0;i<16;i++)b[17-i]="0123456789abcdef"[(v>>(i*4))&15];serial(b);}
void panic(const char*s){serial("[PANIC] ");serial(s);serial("\n");__asm__ volatile("cli");for(;;)__asm__ volatile("hlt");}
void arch_init(void){
 uint64_t base=(uint64_t)&tss;uint64_t limit=sizeof(tss)-1;
 gdt[5]=(limit&0xffff)|((base&0xffffff)<<16)|(0x89ull<<40)|((limit&0xf0000)<<32)|((base&0xff000000)<<32);gdt[6]=base>>32;
 tss.iomap=sizeof(tss);tss.ist[0]=(uint64_t)(fault_stack+sizeof(fault_stack));Descriptor gd={sizeof(gdt)-1,(uint64_t)gdt};load_gdt(&gd);
 for(unsigned i=0;i<256;i++){uint64_t a=(uint64_t)isr_table[i];idt[i]=(Gate){a&65535,8,i==8?1:0,i==128?0xee:0x8e,(a>>16)&65535,a>>32,0};}
 Descriptor id={sizeof(idt)-1,(uint64_t)idt};__asm__ volatile("lidt %0"::"m"(id));
 uint64_t efer;uint32_t lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080));efer=((((uint64_t)hi<<32)|lo)|(1ull<<11))&~(1ull<<14);__asm__ volatile("wrmsr"::"a"((uint32_t)efer),"d"((uint32_t)(efer>>32)),"c"(0xc0000080));
 /* Eager x87/MMX/SSE context only. Never inherit firmware lazy/XSAVE policy. */
 uint32_t eax,ebx,ecx,edx;__asm__ volatile("cpuid":"=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(1),"c"(0));
 if((edx&0x07000001u)!=0x07000001u)panic("x87/FXSR/SSE2 required");
 uint64_t cr0,cr4;__asm__ volatile("mov %%cr0,%0":"=r"(cr0));cr0=(cr0|((1ull<<16)|(1ull<<5)|(1ull<<1)))&~((1ull<<2)|(1ull<<3));__asm__ volatile("mov %0,%%cr0"::"r"(cr0):"memory");
 __asm__ volatile("mov %%cr4,%0":"=r"(cr4));cr4=(cr4|(1ull<<9)|(1ull<<10))&~(1ull<<18);__asm__ volatile("mov %0,%%cr4"::"r"(cr4):"memory");
 /* PIC, PIT channel 0: 100Hz; all other IRQs remain masked. */
 outb(0x20,0x11);outb(0xa0,0x11);outb(0x21,32);outb(0xa1,40);outb(0x21,4);outb(0xa1,2);outb(0x21,1);outb(0xa1,1);outb(0x21,0xee);outb(0xa1,0xff);outb(0x3f9,1);
 outb(0x43,0x36);outb(0x40,11932&255);outb(0x40,11932>>8);
}
void arch_save(Task*t){__asm__ volatile("fxsave64 %0":"=m"(t->fxstate)::"memory");}
void arch_task(Task*t){
 tss.rsp0=(uint64_t)(t->kstack+sizeof(t->kstack));__asm__ volatile("mov %0,%%cr3"::"r"(t->cr3):"memory");
 /* Clear pending x87 status and legacy pointer metadata before restoring.
  * Some FXRSTOR implementations omit exception pointers when ES is clear.
  * FLDENV writes zero pointers without executing an FP memory operation that
  * would itself expose an internal code/data address. */
 __asm__ volatile("fninit; fldenv %1; fxrstor64 %0"::"m"(t->fxstate),"m"(empty_fpenv):"memory");
}
