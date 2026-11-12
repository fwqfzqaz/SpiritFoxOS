#include "kernel.h"
#include "arguments.h"
#include "pid.h"
/* Core 0 微内核主逻辑：任务表、启动校验、系统调用分发与中断调度。
 * 内核不解析路径/ELF/CBOR/包格式，只提供内存、IPC、设备与 ACPI/VT-d 机制。 */
Task tasks[SF_MAX_THREADS];int cpu_currents[SF_MAX_CPUS];KernelLock kernel_lock;uint64_t ticks;
static uint64_t cpu_window_end;
static uint32_t next_pid=1;static uint64_t bundle_phys,bundle_bytes,boot_cr3;
/* Framebuffer：固件提供的显示内存，经 PID1 授权后可被单一客户端映射。 */
static uint64_t fb_physical,fb_bytes,fb_width,fb_height,fb_pitch;
static uint32_t fb_bpp,fb_grant_pid;
static void kill_process(uint32_t,int);
/* UEFI 传递的内存描述符（与 boot/efi_stub 的 SfBootInfo.memory_map 对齐）。 */
typedef struct {uint64_t physical,virtual_address,pages,attribute;uint32_t type,reserved;} BootMemory;
extern uint8_t kernel_end[];
/* 校验 [start,start+bytes) 是否落在固件标记为可用（type 1/2）且预留的内存区间内，
 * 同时强制低于 1 GiB 线——内核只信任固件已保留的低端内存。 */
static int boot_memory(SfBootInfo*b,uint64_t start,uint64_t bytes){
 if(start>=0x40000000ull||!bytes||bytes>0x40000000ull-start)return 0;
 BootMemory*map=(void*)b->memory_map;for(uint64_t i=0;i<b->memory_map_entry_count;i++){BootMemory*m=&map[i];if(m->pages>UINT64_MAX/4096)continue;uint64_t n=m->pages*4096;if((m->type==1||m->type==2)&&start>=m->physical&&start-m->physical<=n&&bytes<=n-(start-m->physical))return 1;}return 0;
}
/* 校验装载计划：尺寸/段数/基址/参数包合法性，段对齐与边界（写入必须落在
 * 镜像窗口内）、段间页对齐互斥重叠检查、入口点必须落在可执行段（flags==5）内。
 * 有父任务时源数据必须位于父任务用户区间；无父任务（bootstrap）时源数据必须
 * 位于 1 GiB 以下的内核保留区。任何越界或重叠都拒绝——这是进程装载的唯一入口。 */
static int valid_plan(const SfLoadPlan*p,Task*parent){
 if(p->size!=sizeof(*p)||!p->count||p->count>8||!sf_image_base_valid(p->image_base)||sf_arguments_validate(&p->arguments)||p->grant_device>65536||(p->device_rights&~SF_DEVICE_WRITE)||(!p->grant_device&&p->device_rights))return 0;int entry=0;
 for(unsigned i=0;i<p->count;i++){const SfLoadSegment*s=&p->segments[i];
  if((s->rva&4095)||s->data>UINT64_MAX-s->bytes||!s->bytes||s->bytes>SF_IMAGE_MAX||s->reserved)return 0;
  /* bytes is the complete zero-extended segment, supplied by the loader. */
  if(s->rva>=SF_IMAGE_MAX||s->bytes>SF_IMAGE_MAX-s->rva||(s->flags!=1&&s->flags!=3&&s->flags!=5))return 0;
  if(parent&&!user_range(parent,s->data,s->bytes,0))return 0;
  if(!parent&&(s->data>0x40000000ull||s->bytes>0x40000000ull-s->data||s->data<(uint64_t)kernel_end))return 0;
  uint64_t end=(s->rva+s->bytes+4095)&~4095ull;
  for(unsigned j=0;j<i;j++)if(s->rva<((p->segments[j].rva+p->segments[j].bytes+4095)&~4095ull)&&p->segments[j].rva<end)return 0;
  if(s->flags==5&&p->entry>=s->rva&&p->entry-s->rva<s->bytes)entry=1;
 }return entry;
}
/* 创建任务（带私有页预算）：分配 PID 与任务槽、建立独立页表、逐页复制/清零
 * 镜像段、铺设用户栈与参数页，按计划授予 manager/service/设备能力。
 * 失败路径必须完整回滚（设备释放、IPC 清理、页表与页归还），并标记 T_DEAD。 */
int task_new_limited(const SfLoadPlan*p,int bootstrap,uint32_t pages){
 if(pages>SF_PAGE_LIMIT_MAX)return SF_INVALID;
 Task*parent=bootstrap?0:&tasks[current];if(!valid_plan(p,parent)||(!parent&&p->image_base&&p->image_base!=SF_BASE))return SF_INVALID;uint64_t image_base=p->image_base?p->image_base:SF_BASE;
 int result=SF_NO_SPACE;
 unsigned procs=0;for(unsigned i=0;i<SF_MAX_THREADS;i++)if(tasks[i].main&&(tasks[i].state==T_READY||tasks[i].state==T_BLOCKED))procs++;
 if(!bootstrap&&procs>=SF_MAX_TASKS)return SF_NO_SPACE;
 int slot=-1;for(unsigned i=0;i<SF_MAX_THREADS;i++)if(tasks[i].state==T_EMPTY||tasks[i].state==T_DEAD){slot=i;break;}if(slot<0)return SF_NO_SPACE;
 long pid=sf_pid_take(&next_pid);if(pid<0)return (int)pid;
 Task*t=&tasks[slot];/* Preserve capability generations across slot reuse. */
 for(unsigned i=0;i<32;i++)t->caps[i].endpoint=0;t->pid=(uint32_t)pid;t->state=T_EMPTY;t->device_io=0;t->device_bdf=0;t->device_fault=0;t->device_modern=0;t->device_kind=0;t->device_slot=UINT32_MAX;t->irq_waiting=0;
 t->priority=SF_PRIORITY_DEFAULT;t->cap_limit=SF_CAP_LIMIT_MAX;t->page_limit=pages;t->page_count=0;t->ready_since=ticks;
 t->tid=1;t->main=1;t->main_slot=(uint32_t)slot;t->thread_slot=0;t->cpu=-1;
 t->thread_limit=SF_THREAD_LIMIT_MAX;t->thread_count=1;t->thread_slots=1;t->next_tid=1;t->cpu_budget=0;t->cpu_used=0;
 /* FXSAVE64 layout: empty x87 tags, zero physical payloads/XMM/pointers,
  * default round-to-nearest controls. FNINIT alone leaves data registers. */
 /* The admitted/reused slot is not the live parent kernel stack.
  * Clear retained bytes here; never erase an exiting task on its active stack. */
 memset(t->kstack,0,sizeof(t->kstack));
 memset(t->fxstate,0,sizeof(t->fxstate));t->fxstate[0]=0x7f;t->fxstate[1]=3;t->fxstate[24]=0x80;t->fxstate[25]=0x1f;
 t->cr3=space_new(t->pid);if(!t->cr3)goto fail;
 for(unsigned i=0;i<p->count;i++){const SfLoadSegment*s=&p->segments[i];for(uint64_t off=0;off<s->bytes;off+=4096){void*page=page_alloc(t->pid);if(!page||space_map(t,image_base+s->rva+off,(uint64_t)page,s->flags))goto fail;size_t n=s->bytes-off;if(n>4096)n=4096;if(parent){if(copy_in(parent,page,s->data+off,n))goto fail;}else memcpy(page,(void*)(s->data+off),n);}}
 for(uint64_t off=4096;off<=65536;off+=4096){void*page=page_alloc(t->pid);if(!page||space_map(t,SF_STACK-off,(uint64_t)page,3))goto fail;}
 void*arg_page=page_alloc(t->pid);if(!arg_page||space_map(t,SF_ARGS,(uint64_t)arg_page,1))goto fail;
 SfBootArgs*args=arg_page;args->size=sizeof(*args)+p->arguments.bytes;args->abi_major=1;args->pid=t->pid;args->stdout_cap=1;args->stderr_cap=1;
 args->argc=p->arguments.count;args->envc=p->arguments.env_count;
 if(args->argc)args->argv_offset=sizeof(*args);if(args->envc)args->env_offset=sizeof(*args)+args->argc*4;
 if(p->arguments.bytes)memcpy((uint8_t*)args+sizeof(*args),p->arguments.data,p->arguments.bytes);
 if(bootstrap||p->grant_bundle){for(uint64_t off=0;off<bundle_bytes;off+=4096)if(space_map(t,SF_BUNDLE+off,bundle_phys+off,1))goto fail;args->bundle=SF_BUNDLE;args->bundle_bytes=bundle_bytes;}
 if(parent){int h;if(p->grant_manager){h=ipc_grant(parent,t,p->grant_manager,RIGHT_SEND);if(h<0)goto fail;args->manager=h;}if(p->grant_service){if(p->service_rights&~7)goto fail;h=ipc_grant(parent,t,p->grant_service,p->service_rights);if(h<0)goto fail;args->service=h;}}
 if(p->grant_device){int r=device_grant(t,p->grant_device,p->device_rights,args);if(r){result=r;goto fail;}}
 t->frame=(Frame*)(t->kstack+sizeof(t->kstack)-sizeof(Frame));memset(t->frame,0,sizeof(Frame));t->frame->rip=image_base+p->entry;t->frame->cs=0x1b;t->frame->ss=0x23;t->frame->rflags=0x202;t->frame->rsp=SF_STACK-8;t->frame->rdi=SF_ARGS;t->frame->rsi=args->size;t->ready_since=ticks;t->state=T_READY;t->exit_status=0;return t->pid;
fail: device_release(t);ipc_cleanup(t);pages_free(t->pid);device_forget(t);t->state=T_DEAD;t->exit_status=result;return result;
}
int task_new(const SfLoadPlan*p,int bootstrap){return task_new_limited(p,bootstrap,SF_PAGE_LIMIT_MAX);}
/* 任务终结：释放设备与 IPC，若终结的是当前任务则先切回内核页表再归还其页，
 * 最后遗忘设备绑定；exit_status 供父任务通过 SYS_TASK_STATUS 观测。 */
void task_dead(Task*t,int status){/* 先撤销共享内存引用，再归还页池，避免他进程 PTE 指向已回收页。 */
 shm_release_pid(t->pid);device_release(t);ipc_cleanup(t);t->state=T_DEAD;t->exit_status=status;t->cpu=-1;
#ifndef SF_HOST_TEST
 int ci=(int)cpu_index();if(cpu_currents[ci]>=0&&tasks[cpu_currents[ci]].pid==t->pid)__asm__ volatile("mov %0,%%cr3"::"r"(boot_cr3):"memory");
#endif
 pages_free(t->pid);device_forget(t);}
static uint64_t thread_stack(const Task*t){return SF_THREAD_STACK_BASE+(uint64_t)t->thread_slot*SF_THREAD_STACK_BYTES;}
/* 同级线程退出：释放自己的栈映射与线程槽，主线程/其他线程继续运行。 */
static void thread_exit(Task*t,int status){
 Task*m=task_main(t);
 space_unmap_range(t,thread_stack(t),SF_THREAD_STACK_BYTES);
 t->state=T_DEAD;t->exit_status=status;t->cpu=-1;
 if(m->thread_count)m->thread_count--;
 m->thread_slots&=~(1u<<t->thread_slot);
}
/* 进程回收：先让其他 CPU 上的线程停下，再回收主线程与共享页。当前 CPU
 * 若正运行该进程的任一线程，必须先切回内核页表，才能释放共享页表。 */
static void kill_process(uint32_t pid,int status){
 Task*m=0;for(unsigned i=0;i<SF_MAX_THREADS;i++)if(tasks[i].main&&tasks[i].pid==pid&&tasks[i].state!=T_EMPTY){m=&tasks[i];break;}
 if(!m||m->state==T_DEAD)return;int ci=(int)cpu_index();
#ifndef SF_HOST_TEST
 if(cpu_currents[ci]>=0&&tasks[cpu_currents[ci]].pid==pid)__asm__ volatile("mov %0,%%cr3"::"r"(boot_cr3):"memory");
#endif
 Task*killed[SF_MAX_THREADS];unsigned nk=0;
 for(unsigned i=0;i<SF_MAX_THREADS;i++){
  Task*x=&tasks[i];if(x->pid!=pid||x->state==T_EMPTY||x->state==T_DEAD)continue;
  x->state=T_DEAD;x->exit_status=status;
  if(x->cpu==ci){x->cpu=-1;continue;}
  killed[nk++]=x;
 }
 smp_preempt_all();
 for(unsigned k=0;k<nk;k++){
  for(unsigned spin=0;killed[k]->cpu>=0;spin++){
   if(spin>10000000u)panic("thread stop timeout");
   kernel_lock_release();__asm__ volatile("pause");kernel_lock_acquire();
  }
 }
 for(unsigned k=0;k<nk;k++){
  if(!killed[k]->main){space_unmap_range(killed[k],thread_stack(killed[k]),SF_THREAD_STACK_BYTES);m->thread_slots&=~(1u<<killed[k]->thread_slot);if(m->thread_count)m->thread_count--;}
 }
 m->thread_slots=1;m->thread_count=1;task_dead(m,status);
}
int task_schedulable(const Task*t){
 if(t->state!=T_READY||t->cpu!=-1)return 0;
 const Task*m=task_main_c(t);
 return m->cpu_budget==0||m->cpu_used<m->cpu_budget;
}
void task_kill(uint32_t pid,int status){kill_process(pid,status);}
void task_wake(Task*t){t->state=T_READY;t->ready_since=ticks;smp_preempt_all();}
void cpu_account(void){int c=cpu_currents[cpu_index()];if(c<0)return;Task*m=task_main(&tasks[c]);if(m->cpu_budget&&m->cpu_used<UINT64_MAX)m->cpu_used++;}
void cpu_window_reset(void){if(ticks<cpu_window_end)return;for(unsigned i=0;i<SF_MAX_THREADS;i++)if(tasks[i].main)tasks[i].cpu_used=0;cpu_window_end=ticks+SF_CPU_WINDOW_TICKS;}
#ifndef SF_HOST_TEST
/* 每 CPU 空闲栈：空闲等待不能继续使用刚被换出任务的内核栈，否则该任务
 * 被其他 CPU 调度后两个 CPU 会复用同一 16KB 栈并互相覆盖中断帧。 */
static uint8_t idle_stacks[SF_MAX_CPUS][16384] __attribute__((aligned(16)));
void idle_loop(void){
 for(;;){__asm__ volatile("sti; hlt; cli":::"memory");kernel_lock_acquire();Frame*f=schedule();kernel_lock_release();arch_enter(f);}
}
#endif
/* 调度入口：构建就绪表并调用 sf_sched_pick（Rust 实现）挑选下一任务，
 * 校验返回栈指针规范后经 arch_task 切换上下文。运行中的任务不累计等待
 * 老化时间（计时起点在唤醒时重置）；无就绪任务时切到空闲栈后释放全局锁。 */
Frame *schedule(void){
 int ci=(int)cpu_index(),prev=cpu_currents[ci];
 if(prev>=0){Task*cur=&tasks[prev];if(cur->state==T_READY)cur->ready_since=ticks;cur->cpu=-1;}
 for(;;){SfSchedEntry entries[SF_MAX_THREADS];
  for(unsigned j=0;j<SF_MAX_THREADS;j++)entries[j]=(SfSchedEntry){tasks[j].ready_since,tasks[j].priority,(uint32_t)task_schedulable(&tasks[j])};
  int i=sf_sched_pick(entries,SF_MAX_THREADS,prev,ticks);if(i>=0){
   Task*t=&tasks[i];uint64_t rsp=t->frame->rsp;
   /* An arbitrary ring-3 RSP can make IRET fault in ring 0. Validate before
    * returning, independently of the emulator's IRET canonicality checks. */
   if(rsp>=0x0000800000000000ull&&rsp<0xffff800000000000ull){
    if(t->pid==1)panic("initial task return frame");
    if(t->main)kill_process(t->pid,-113);else thread_exit(t,-113);
    cpu_currents[ci]=-1;prev=-1;continue;
   }
   cpu_currents[ci]=i;t->cpu=ci;t->ready_since=ticks;arch_task(t);return t->frame;}
  cpu_currents[ci]=-1;
#ifndef SF_HOST_TEST
  enter_idle((uint64_t)idle_stacks[ci]+sizeof(idle_stacks[ci]),prev>=0?&tasks[prev].cpu:0,space_ap_cr3());
#else
  kernel_lock_release();__asm__ volatile("sti; hlt; cli");kernel_lock_acquire();prev=-1;
#endif
 }
}
/* 系统调用分发：rax 为调用号，rdi/rsi/rdx 为参数。
 * 大多数管理类调用仅限 PID 1（sf-init）；参数用 copy_in 从用户空间校验复制，
 * 返回值写回 rax。default 分支转入 IPC 系统调用（send/recv/grant 等）。 */
static Task *pid_main(uint64_t pid,int alive){for(unsigned i=0;i<SF_MAX_THREADS;i++){Task*x=&tasks[i];if(!x->main||x->pid!=pid||x->state==T_EMPTY)continue;if(!alive||x->state==T_READY||x->state==T_BLOCKED)return x;}return 0;}
static long syscall(Frame*f){Task*t=&tasks[current];uint64_t a=f->rdi,b=f->rsi,c=f->rdx;
 switch(f->rax){
 case SYS_EXIT:
  if(t->main){if(t->pid==1)panic("initial task exited");kill_process(t->pid,(int)a);return 0;}
  thread_exit(t,(int)a);return 0;
 case SYS_YIELD:return 0;
 case SYS_LOG:{if(b>512||!user_range(t,a,b,0))return SF_DENIED;char buf[513];copy_in(t,buf,a,b);buf[b]=0;serial(buf);return b;}
 case SYS_TICKS:return ticks;
 case SYS_IPC_STATS:{if(t->pid!=1)return SF_DENIED;if(b!=sizeof(SfIpcStats)||c)return SF_INVALID;SfIpcStats info;ipc_stats_snapshot(&info);return copy_out(t,a,&info,sizeof(info));}
 case SYS_TASK_PAGE_LIMIT:{
  if(t->pid!=1)return SF_DENIED;
  if(!a||a>INT32_MAX||(c!=SF_LIMIT_QUERY&&c!=SF_LIMIT_SET&&c!=SF_LIMIT_USED)||b>SF_PAGE_LIMIT_MAX||(c!=SF_LIMIT_SET&&b))return SF_INVALID;
  Task*x=pid_main(a,1);if(!x)return SF_NOT_FOUND;
  if(c==SF_LIMIT_USED)return x->page_count;
  if(c==SF_LIMIT_SET){if(x->page_count>b)return SF_BUSY;x->page_limit=(uint32_t)b;}
  return x->page_limit;
 }
 case SYS_TASK_CAP_LIMIT:{
  if(t->pid!=1)return SF_DENIED;
  if(!a||a>INT32_MAX||(c!=SF_LIMIT_QUERY&&c!=SF_LIMIT_SET&&c!=SF_LIMIT_USED)||b>SF_CAP_LIMIT_MAX||(c!=SF_LIMIT_SET&&b))return SF_INVALID;
  Task*x=pid_main(a,1);if(!x)return SF_NOT_FOUND;
  if(c==SF_LIMIT_USED)return task_caps_used(x);
  if(c==SF_LIMIT_SET){if(task_caps_used(x)>b)return SF_BUSY;x->cap_limit=(uint32_t)b;}
  return x->cap_limit;
 }
 case SYS_TASK_PRIORITY:{
  if(t->pid!=1)return SF_DENIED;
  if(!a||a>INT32_MAX||(c!=SF_PRIORITY_QUERY&&c!=SF_PRIORITY_SET)||b>SF_PRIORITY_MAX||(c==SF_PRIORITY_QUERY&&b))return SF_INVALID;
  Task*x=pid_main(a,1);if(!x)return SF_NOT_FOUND;
  if(c==SF_PRIORITY_SET)for(unsigned i=0;i<SF_MAX_THREADS;i++)if(tasks[i].pid==a&&tasks[i].state!=T_EMPTY&&tasks[i].state!=T_DEAD)tasks[i].priority=(uint32_t)b;
  return x->priority;
 }
 case SYS_THREAD_LIMIT:{
  if(t->pid!=1)return SF_DENIED;
  if(!a||a>INT32_MAX||(c!=SF_LIMIT_QUERY&&c!=SF_LIMIT_SET&&c!=SF_LIMIT_USED)||b>SF_THREAD_LIMIT_MAX||(c==SF_LIMIT_SET?!b:(c!=SF_LIMIT_SET&&b)))return SF_INVALID;
  Task*x=pid_main(a,1);if(!x)return SF_NOT_FOUND;
  if(c==SF_LIMIT_USED)return x->thread_count;
  if(c==SF_LIMIT_SET){if(x->thread_count>b)return SF_BUSY;x->thread_limit=(uint32_t)b;}
  return x->thread_limit;
 }
 case SYS_TASK_CPU_BUDGET:{
  if(t->pid!=1)return SF_DENIED;
  if(!a||a>INT32_MAX||(c!=SF_LIMIT_QUERY&&c!=SF_LIMIT_SET&&c!=SF_LIMIT_USED)||b>SF_CPU_WINDOW_TICKS||(c!=SF_LIMIT_SET&&b))return SF_INVALID;
  Task*x=pid_main(a,1);if(!x)return SF_NOT_FOUND;
  if(c==SF_LIMIT_USED)return (long)x->cpu_used;
  if(c==SF_LIMIT_SET){/* 0 表示不限，等价于抬高上限，不因已用大于 0 而拒绝。 */if(b&&x->cpu_used>b)return SF_BUSY;x->cpu_budget=(uint32_t)b;}
  return x->cpu_budget;
 }
 case SYS_THREAD_CREATE:{
  if(b||c)return SF_INVALID;SfThreadRequest r;if(copy_in(t,&r,a,sizeof(r)))return SF_DENIED;
  if(r.size!=sizeof(r)||r.flags)return SF_INVALID;Task*m=task_main(t);
  if(m->thread_limit>SF_THREAD_LIMIT_MAX)m->thread_limit=SF_THREAD_LIMIT_MAX;
  if(m->thread_count>=m->thread_limit)return SF_NO_SPACE;
  unsigned ts=1;while(ts<SF_THREAD_LIMIT_MAX&&(m->thread_slots&(1u<<ts)))ts++;
  if(ts>=SF_THREAD_LIMIT_MAX)return SF_NO_SPACE;
  if(!user_exec(m,r.entry))return SF_DENIED;
  int slot=-1;for(unsigned i=0;i<SF_MAX_THREADS;i++)if(tasks[i].state==T_EMPTY||tasks[i].state==T_DEAD){slot=i;break;}if(slot<0)return SF_NO_SPACE;
  Task*n=&tasks[slot];for(unsigned i=0;i<32;i++)n->caps[i].endpoint=0;
  n->pid=m->pid;n->cr3=m->cr3;n->state=T_EMPTY;n->wait_endpoint=0;n->recv_buffer=0;n->wait_deadline=0;n->frame=0;n->exit_status=0;n->device_io=0;n->device_bdf=0;n->device_fault=0;n->device_modern=0;n->device_kind=0;n->device_slot=UINT32_MAX;n->irq_waiting=0;
  n->priority=m->priority;n->cap_limit=0;n->page_limit=0;n->page_count=0;
  n->tid=++m->next_tid;n->main=0;n->main_slot=m->main_slot;n->thread_slot=ts;n->cpu=-1;
  n->thread_limit=n->thread_count=n->thread_slots=n->next_tid=n->cpu_budget=0;n->cpu_used=0;n->ready_since=ticks;
  memset(n->kstack,0,sizeof(n->kstack));memset(n->fxstate,0,sizeof(n->fxstate));n->fxstate[0]=0x7f;n->fxstate[1]=3;n->fxstate[24]=0x80;n->fxstate[25]=0x1f;
  uint64_t base=thread_stack(n);unsigned mapped=0;
  for(uint64_t off=0;off<SF_THREAD_STACK_BYTES;off+=4096){void*p=page_alloc(n->pid);if(!p){space_unmap_range(n,base,mapped);n->state=T_DEAD;return SF_NO_SPACE;}if(space_map(n,base+off,(uint64_t)p,3)){page_free_phys(n->pid,(uint64_t)p);space_unmap_range(n,base,mapped);n->state=T_DEAD;return SF_NO_SPACE;}mapped+=4096;}
  n->frame=(Frame*)(n->kstack+sizeof(n->kstack)-sizeof(Frame));memset(n->frame,0,sizeof(Frame));n->frame->rip=r.entry;n->frame->cs=0x1b;n->frame->ss=0x23;n->frame->rflags=0x202;n->frame->rsp=base+SF_THREAD_STACK_BYTES-8;n->frame->rdi=r.argument;
  m->thread_slots|=1u<<ts;m->thread_count++;n->state=T_READY;n->exit_status=0;task_wake(n);return (long)n->tid;
 }
 case SYS_SPAWN_LIMITED:{
  if(t->pid!=1)return SF_DENIED;if(b>SF_PAGE_LIMIT_MAX||c)return SF_INVALID;
  SfLoadPlan plan;if(copy_in(t,&plan,a,sizeof(plan)))return SF_DENIED;return task_new_limited(&plan,0,(uint32_t)b);
 }
 case SYS_SPAWN:{if(t->pid!=1)return SF_DENIED;SfLoadPlan plan;if(copy_in(t,&plan,a,sizeof(plan)))return SF_DENIED;return task_new(&plan,0);}
 case SYS_KILL:{
  if(t->pid!=1||a==1)return SF_DENIED;Task*x=pid_main(a,0);if(!x)return SF_NOT_FOUND;kill_process((uint32_t)a,(int)b);return 0;
 }
 case SYS_TASK_STATUS:{
  if(t->pid!=1)return SF_DENIED;Task*x=pid_main(a,0);if(!x)return SF_NOT_FOUND;
  if(x->state!=T_DEAD)return SF_TASK_RUNNING;return (long)x->exit_status;
 }
 case SYS_CONSOLE_READ:if(t->pid!=1)return SF_DENIED;return console_read();
 case SYS_POINTER_READ:if(t->pid!=1)return SF_DENIED;return ps2_mouse_read((int)a);
 case SYS_SHUTDOWN:if(t->pid!=1)return SF_DENIED;__asm__ volatile("outl %0,%1"::"a"((uint32_t)a),"Nd"((uint16_t)0xf4));outb(0x604,0);return 0;
 case SYS_MAP_TEST:return SF_DENIED;
 case SYS_PCI_READ:if(t->pid!=1)return SF_DENIED;return pci_read(a,b);
 case SYS_PORT_IO:return device_io(task_main(t),a,b,c);
 case SYS_DEVICE_RELEASE:{
  if(!a)return device_release(task_main(t));if(t->pid!=1)return SF_DENIED;Task*x=pid_main(a,0);return x?device_release(x):SF_NOT_FOUND;
 }
 case SYS_DMA_STATUS:{if(t->pid!=1)return SF_DENIED;if(b!=sizeof(SfDmaStatus))return SF_INVALID;SfDmaStatus status;device_status(&status);return copy_out(t,a,&status,sizeof(status));}
 case SYS_IRQ_ACQUIRE:return device_irq_acquire(t);
 case SYS_IRQ_RELEASE:return device_irq_release(t);
 case SYS_IRQ_WAIT:{int r=device_irq_wait(t,(uint32_t)a);if(r==SF_WOULD_BLOCK){t->state=T_BLOCKED;return 0;}return r;}
 /* PID1 把显示内存的使用权授予单个合成器进程。 */
 case SYS_FB_GRANT:{
  if(t->pid!=1||b!=0||c!=0)return SF_DENIED;
  if(!a||a>INT32_MAX)return SF_INVALID;
  Task*x=pid_main(a,1);if(!x)return SF_NOT_FOUND;
  fb_grant_pid=(uint32_t)a;
  /* 合成器接管屏幕后停掉内核文本控制台，避免启动日志覆盖图形界面。 */
  console_fb_init(0,0,0,0,0,0);
  return SF_OK;
 }
 /* 被授权进程把显示内存映射到 SF_FB，并取得 SfFbInfo。 */
 case SYS_FB_MAP:{
  if(!fb_grant_pid||t->pid!=fb_grant_pid)return SF_DENIED;
  if(b||c)return SF_INVALID;
  if(!fb_bytes)return SF_NOT_FOUND;
  if(!user_range(t,a,sizeof(SfFbInfo),1))return SF_INVALID;
  /* 以本任务页表为准判断是否已映射：PID 槽位复用时不会误跳过映射。 */
  if(!user_phys(t,SF_FB)){int r=space_map_fb(t,SF_FB,fb_physical,fb_bytes);if(r)return r;}
  SfFbInfo info={sizeof(info),(uint32_t)fb_width,(uint32_t)fb_height,(uint32_t)fb_pitch,fb_bpp,0,fb_bytes};
  return copy_out(t,a,&info,sizeof(info));
 }
 case SYS_MMAP:return space_map_anon(task_main(t),a,b);
 case SYS_MUNMAP:{
  uint64_t bytes=b;if((a&4095)||(bytes&4095)||!bytes||bytes>SF_HEAP_BYTES||a<SF_HEAP||a>SF_HEAP+SF_HEAP_BYTES-bytes)return SF_INVALID;
  return space_unmap_range(task_main(t),a,bytes);
 }
 case SYS_SHM_CREATE:case SYS_SHM_MAP:case SYS_SHM_GRANT:case SYS_SHM_CLOSE:
  return shm_syscall(f->rax,a,b,c);
 default:return ipc_syscall(f->rax,a,b,c);
 }
}
/* 中断/异常总入口：vector 32 为时钟（驱动滴答与 IPC 截止时间），36 为串口输入。
 * 内核态异常立即 panic；用户态异常按任务隔离处理（终结该任务，exit_status
 * 为 -100-vector）。vector 128 为系统调用。设备轮询与调度收尾。 */
Frame *interrupt_dispatch(Frame*f){
 kernel_lock_acquire();
 if(f->vector==0x70){
  if(f->cs&3){Task*t=&tasks[current];arch_save(t);t->frame=f;}
  cpu_account();apic_eoi();Frame*r=schedule();kernel_lock_release();return r;
 }
 if(f->vector==0x71){uint64_t cr3;__asm__ volatile("mov %%cr3,%0":"=r"(cr3));__asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");apic_eoi();kernel_lock_release();return f;}
 if(f->vector==255){apic_eoi();kernel_lock_release();return f;}
 /* 设备 INTx：IOAPIC 固定投递到 0x50；固件只接 8259 时走 0x20+line，
  * 从片中断还需先经主片级联 0x22 并分别 EOI。PS/2 鼠标的 IRQ12 在两种
  * 拓扑下都可能落在 0x2c / 0x22 / 0x50，统一先排空 i8042 再按路径 EOI。 */
 if(f->vector==0x50){ps2_irq();device_irq_vector();apic_eoi();}
 else if(f->vector==0x2c){ps2_irq();outb(0xa0,0x20);outb(0x20,0x20);}
 else if(f->vector>=0x28&&f->vector<=0x2f){device_irq_vector();outb(0xa0,0x20);outb(0x20,0x20);}
 else if(f->vector==0x22){ps2_irq();outb(0xa0,0x20);outb(0x20,0x20);}
 if(f->vector==32&&cpu_index()==0){ticks++;outb(0x20,0x20);cpu_window_reset();cpu_account();ipc_deadlines();smp_preempt_all();}
 if(f->vector==36)console_irq();
 /* 空闲 CPU 在 ring0 收到中断时 current 为 -1，不能索引任务表；直接返回
  * 让 idle_loop 重新加锁调度即可。 */
 if((f->cs&3)==0){if(f->vector<32){serial("[CORE] kernel exception ");hex(f->vector);serial(" at ");hex(f->rip);panic("kernel exception");}kernel_lock_release();return f;}
 Task*t=&tasks[current];arch_save(t);t->frame=f;device_poll();if(t->state==T_DEAD){Frame*r=schedule();kernel_lock_release();return r;}
 if(f->vector==128)f->rax=syscall(f);
 else if(f->vector<32){serial("[CORE] isolated user fault pid=");hex(t->pid);serial(" vector=");hex(f->vector);serial("\n");if(t->pid==1)panic("initial task fault");
  __asm__ volatile("mov %0,%%cr3"::"r"(boot_cr3):"memory");kill_process(t->pid,-100-(int)f->vector);}
 Frame*r=schedule();kernel_lock_release();return r;
}
/* 内核入口：逐项校验 UEFI 传递的 BootInfo（magic/尺寸/版本/段数、启动包边界、
 * 内存映射描述符、关键对象均有固件预留），再把 init 段转成装载计划创建
 * PID 1（sf-init），最后经 arch_enter 进入第一个调度循环。 */
void kernel_main(SfBootInfo*b){
 __asm__ volatile("mov %%cr3,%0":"=r"(boot_cr3));
 for(unsigned i=0;i<SF_MAX_CPUS;i++)cpu_currents[i]=-1;
 /* 真机通常没有串口：先把固件 framebuffer 接成文本控制台，后续所有
  * serial()/panic 输出会同时显示在屏幕上。 */
 if((uint64_t)b<0x40000000ull&&b->magic==0x5f1f0f05)console_fb_init(b->framebuffer_base,b->framebuffer_size,b->width,b->height,b->pitch,b->bpp);
 serial("\nSpiritFoxOS Core 0 / ABI 1\n");
 if((uint64_t)b>=0x40000000ull||b->magic!=0x5f1f0f05||b->boot_type!=1||b->size!=sizeof(*b)||b->version!=1||!b->init_segment_count||b->init_segment_count>8)panic("BootInfo rejected");
 if((b->bundle&4095)||!b->bundle_bytes||b->bundle_bytes>4*1024*1024||b->bundle>=0x40000000ull||b->bundle_bytes>0x40000000ull-b->bundle)panic("bundle bounds");
 if(b->memory_map_descriptor_size!=sizeof(BootMemory)||!b->memory_map_entry_count||b->memory_map_entry_count>2048||b->memory_map_size!=b->memory_map_entry_count*sizeof(BootMemory)||b->memory_map>=0x40000000ull||b->memory_map_size>0x40000000ull-b->memory_map)panic("memory map bounds");
 if(!boot_memory(b,(uint64_t)b,sizeof(*b))||!boot_memory(b,b->bundle,b->bundle_bytes))panic("unreserved boot object");
 /* 保存固件 framebuffer 描述；任一约束不满足则视为无图形（不影响串口路径）。 */
 fb_physical=fb_bytes=fb_width=fb_height=fb_pitch=0;fb_bpp=fb_grant_pid=0;
 if(b->framebuffer_base&&!(b->framebuffer_base&4095)&&b->framebuffer_size&&
    b->framebuffer_size<=SF_FB_BYTES&&b->framebuffer_base<=0x000ffffffffff000ull-b->framebuffer_size&&
    b->pitch&&b->width&&b->height&&b->width<=8192&&b->height<=8192&&
    (uint64_t)b->pitch*b->height<=b->framebuffer_size){
  fb_physical=b->framebuffer_base;fb_bytes=b->framebuffer_size;
  fb_width=b->width;fb_height=b->height;fb_pitch=b->pitch;fb_bpp=b->bpp;
  if(space_fb_init(fb_physical,fb_bytes))console_fb_init(0,0,0,0,0,0);
 }else console_fb_init(0,0,0,0,0,0);
 bundle_phys=b->bundle;bundle_bytes=b->bundle_bytes;arch_init();ps2_mouse_init((uint32_t)fb_width,(uint32_t)fb_height);device_init(b);smp_init(b);device_irq_init();SfLoadPlan p={0};p.size=sizeof(p);p.count=b->init_segment_count;p.entry=b->init_entry;
 for(unsigned i=0;i<p.count;i++){SfBootSegment*s=&b->init_segments[i];if(s->reserved||s->file_bytes>s->memory_bytes||(s->physical&4095)||!boot_memory(b,s->physical,(s->memory_bytes+4095)&~4095ull))panic("boot segment");if(s->physical<b->bundle+b->bundle_bytes&&b->bundle<s->physical+s->memory_bytes)panic("boot segment overlaps bundle");for(unsigned j=0;j<i;j++){SfBootSegment*t=&b->init_segments[j];if(s->physical<t->physical+t->memory_bytes&&t->physical<s->physical+s->memory_bytes)panic("overlapping boot segments");}p.segments[i]=(SfLoadSegment){s->rva,s->memory_bytes,s->physical,s->flags,0};}
 if(task_new(&p,1)!=1)panic("bootstrap plan rejected");serial("[CORE] entering isolated sf-init.sfmod, timer=100Hz\n");
 kernel_lock_acquire();Frame*f=schedule();kernel_lock_release();arch_enter(f);
}
