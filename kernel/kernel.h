#ifndef KERNEL_H
#define KERNEL_H
#include "sf.h"
#include "sched_policy.h"
#define NX (1ull<<63)
typedef struct {uint64_t r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rbx,rdx,rcx,rax,vector,error,rip,cs,rflags,rsp,ss;} Frame;
enum {T_EMPTY,T_READY,T_BLOCKED,T_DEAD};
/* 能力的派生树锚点：根能力 parent_pid 为 0；派生/转移得到的能力记录父
 * 能力所在的 PID、槽位和该槽位的 generation。generation 防止父槽复用后
 * 把旧的派生关系错误地绑定到新能力。 */
typedef struct {uint32_t endpoint,epoch,rights,generation;uint32_t parent_pid,parent_slot,parent_generation;} Cap;
typedef struct {
 uint32_t pid,state,wait_endpoint;int32_t exit_status;
 uint64_t cr3,recv_buffer,wait_deadline;Frame *frame;
 uint32_t device_io,device_bdf,device_fault;
 uint32_t device_modern,device_slot,irq_waiting;
 uint32_t priority,cap_limit,page_limit,page_count;uint64_t ready_since;
 uint32_t tid,main,main_slot,thread_slot;int32_t cpu;
 uint32_t thread_limit,thread_count,thread_slots,next_tid,cpu_budget;uint64_t cpu_used;
 Cap caps[32];uint8_t fxstate[512] __attribute__((aligned(16)));
 uint8_t kstack[16384] __attribute__((aligned(16)));
} Task;
_Static_assert(offsetof(Task,fxstate)%16==0&&sizeof(Task)%16==0,"FXSAVE task alignment");
_Static_assert(SF_CAP_LIMIT_MAX==sizeof(((Task*)0)->caps)/sizeof(Cap),"capability limit/table size");
/* 每 CPU 运行态；cpu_index() 经 GS_BASE 定位本核。 */
typedef struct {uint32_t index,apic_id,online,bsp;uint64_t boot_stack;} Cpu;
typedef struct {uint32_t ticket,serving;} KernelLock;
extern Task tasks[SF_MAX_THREADS];extern int cpu_currents[SF_MAX_CPUS];extern uint64_t ticks;
extern KernelLock kernel_lock;extern Cpu cpus[SF_MAX_CPUS];
#ifdef SF_HOST_TEST
static inline unsigned cpu_index(void){return 0;}
#define smp_preempt_all() ((void)0)
#define smp_tlb_flush_all() ((void)0)
#define smp_init(b) ((void)(b))
#define apic_eoi() ((void)0)
static inline unsigned smp_online(void){return 1;}
#else
unsigned cpu_index(void);void smp_preempt_all(void);void smp_tlb_flush_all(void);
void smp_init(SfBootInfo*);void apic_eoi(void);unsigned smp_online(void);
#endif
/* 主线程承载进程级状态：能力、页/能力/线程/CPU 配额、设备绑定。 */
static inline Task *task_main(Task*t){return &tasks[t->main_slot];}
static inline const Task *task_main_c(const Task*t){return &tasks[t->main_slot];}
#define current (cpu_currents[cpu_index()])
#ifdef SF_HOST_TEST
static inline void kernel_lock_acquire(void){}
static inline void kernel_lock_release(void){}
#else
static inline void kernel_lock_acquire(void){__asm__ volatile("cli" ::: "memory");uint32_t t=__atomic_fetch_add(&kernel_lock.ticket,1,__ATOMIC_ACQUIRE);while(__atomic_load_n(&kernel_lock.serving,__ATOMIC_ACQUIRE)!=t)__asm__ volatile("pause");}
static inline void kernel_lock_release(void){__atomic_fetch_add(&kernel_lock.serving,1,__ATOMIC_RELEASE);}
#endif
static inline unsigned task_caps_used(const Task*t){const Task*m=task_main_c(t);unsigned count=0;for(unsigned i=0;i<SF_CAP_LIMIT_MAX;i++)if(m->caps[i].endpoint)count++;return count;}
void serial(const char*);void hex(uint64_t);void panic(const char*);
void console_irq(void);long console_read(void);
void arch_init(void);void arch_cpu_init(unsigned);void arch_load_idt(void);void arch_task(Task*);void arch_save(Task*);void arch_enter(Frame*) __attribute__((noreturn));
/* 空闲等待：enter_idle 切换到每 CPU 独立栈并释放全局锁，idle_loop 在
 * sti;hlt 后重新抢占全局锁调度；两者都不返回。 */
void idle_loop(void) __attribute__((noreturn));
void enter_idle(uint64_t,int*,uint64_t) __attribute__((noreturn));
void *page_alloc(uint32_t);void page_free_phys(uint32_t,uint64_t);void pages_free(uint32_t);uint64_t space_new(uint32_t);
int space_map(Task*,uint64_t,uint64_t,uint32_t);int user_range(Task*,uint64_t,uint64_t,int);
void *user_phys(Task*,uint64_t);int copy_in(Task*,void*,uint64_t,size_t);int copy_out(Task*,uint64_t,const void*,size_t);
int space_unmap_range(Task*,uint64_t,uint64_t);int user_exec(Task*,uint64_t);
void space_apic_init(void);uint64_t space_ap_cr3(void);int space_ioapic_init(void);
int task_new(const SfLoadPlan*,int);int task_new_limited(const SfLoadPlan*,int,uint32_t);void task_dead(Task*,int);Frame *schedule(void);
int task_schedulable(const Task*);void task_wake(Task*);void task_kill(uint32_t,int);void cpu_account(void);void cpu_window_reset(void);
long ipc_syscall(uint64_t,uint64_t,uint64_t,uint64_t);void ipc_cleanup(Task*);
void ipc_stats_snapshot(SfIpcStats*);
int ipc_grant(Task*,Task*,uint32_t,uint32_t);void ipc_deadlines(void);
long pci_read(uint64_t,uint64_t);long device_io(Task*,uint64_t,uint64_t,uint64_t);
int device_grant(Task*,uint32_t,uint32_t,SfBootArgs*);int device_release(Task*);
void device_forget(Task*);
void device_init(SfBootInfo*);void device_poll(void);int device_status(SfDmaStatus*);
void device_irq_init(void);int device_irq_acquire(Task*);int device_irq_release(Task*);
int device_irq_wait(Task*,uint32_t);void device_irq_vector(void);
int space_mmio_init(uint64_t);
int space_device_mmio(uint64_t);int space_map_mmio(Task*,uint64_t,uint64_t);
void iommu_init(SfBootInfo*);unsigned iommu_mode(void);
int iommu_grant(uint32_t,uint64_t,uint64_t*);int iommu_revoke(uint32_t);int iommu_active(void);
int iommu_fault(uint32_t*,uint32_t*,uint64_t*);
static inline void outb(uint16_t p,uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t inb(uint16_t p){uint8_t v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
#endif
