#ifndef KERNEL_H
#define KERNEL_H
#include "sf.h"
#define NX (1ull<<63)
typedef struct {uint64_t r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rbx,rdx,rcx,rax,vector,error,rip,cs,rflags,rsp,ss;} Frame;
enum {T_EMPTY,T_READY,T_BLOCKED,T_DEAD};
typedef struct {uint32_t endpoint,epoch,rights,generation;} Cap;
typedef struct {
 uint32_t pid,state,wait_endpoint;int32_t exit_status;
 uint64_t cr3,recv_buffer,wait_deadline;Frame *frame;
 uint32_t device_io,device_bdf,device_fault;
 uint32_t device_modern;
 Cap caps[32];uint8_t kstack[16384] __attribute__((aligned(16)));
} Task;
extern Task tasks[SF_MAX_TASKS];extern int current;extern uint64_t ticks;
void serial(const char*);void hex(uint64_t);void panic(const char*);
void console_irq(void);long console_read(void);
void arch_init(void);void arch_task(Task*);void arch_enter(Frame*) __attribute__((noreturn));
void *page_alloc(uint32_t);void pages_free(uint32_t);uint64_t space_new(uint32_t);
int space_map(Task*,uint64_t,uint64_t,uint32_t);int user_range(Task*,uint64_t,uint64_t,int);
void *user_phys(Task*,uint64_t);int copy_in(Task*,void*,uint64_t,size_t);int copy_out(Task*,uint64_t,const void*,size_t);
int task_new(const SfLoadPlan*,int);void task_dead(Task*,int);Frame *schedule(void);
long ipc_syscall(uint64_t,uint64_t,uint64_t,uint64_t);void ipc_cleanup(Task*);
int ipc_grant(Task*,Task*,uint32_t,uint32_t);void ipc_deadlines(void);
long pci_read(uint64_t,uint64_t);long device_io(Task*,uint64_t,uint64_t,uint64_t);
int device_grant(Task*,uint32_t,uint32_t,SfBootArgs*);int device_release(Task*);
void device_forget(Task*);
void device_init(SfBootInfo*);void device_poll(void);int device_status(SfDmaStatus*);
int space_mmio_init(uint64_t);
int space_device_mmio(uint64_t);int space_map_mmio(Task*,uint64_t,uint64_t);
void iommu_init(SfBootInfo*);unsigned iommu_mode(void);
int iommu_grant(uint32_t,uint64_t,uint64_t*);int iommu_revoke(uint32_t);int iommu_active(void);
int iommu_fault(uint32_t*,uint32_t*,uint64_t*);
static inline void outb(uint16_t p,uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t inb(uint16_t p){uint8_t v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
#endif
