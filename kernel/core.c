#include "kernel.h"
#include "arguments.h"
#include "pid.h"
Task tasks[SF_MAX_TASKS];int current=-1;uint64_t ticks;
static uint32_t next_pid=1;static uint64_t bundle_phys,bundle_bytes,boot_cr3;
typedef struct {uint64_t physical,virtual_address,pages,attribute;uint32_t type,reserved;} BootMemory;
extern uint8_t kernel_end[];
static int boot_memory(SfBootInfo*b,uint64_t start,uint64_t bytes){
 if(start>=0x40000000ull||!bytes||bytes>0x40000000ull-start)return 0;
 BootMemory*map=(void*)b->memory_map;for(uint64_t i=0;i<b->memory_map_entry_count;i++){BootMemory*m=&map[i];if(m->pages>UINT64_MAX/4096)continue;uint64_t n=m->pages*4096;if((m->type==1||m->type==2)&&start>=m->physical&&start-m->physical<=n&&bytes<=n-(start-m->physical))return 1;}return 0;
}
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
int task_new(const SfLoadPlan*p,int bootstrap){
 Task*parent=bootstrap?0:&tasks[current];if(!valid_plan(p,parent)||(!parent&&p->image_base&&p->image_base!=SF_BASE))return SF_INVALID;uint64_t image_base=p->image_base?p->image_base:SF_BASE;
 int result=SF_NO_SPACE;
 int slot=-1;for(unsigned i=0;i<SF_MAX_TASKS;i++)if(tasks[i].state==T_EMPTY||tasks[i].state==T_DEAD){slot=i;break;}if(slot<0)return SF_NO_SPACE;
 long pid=sf_pid_take(&next_pid);if(pid<0)return (int)pid;
 Task*t=&tasks[slot];/* Preserve capability generations across slot reuse. */
 for(unsigned i=0;i<32;i++)t->caps[i].endpoint=0;t->pid=(uint32_t)pid;t->state=T_EMPTY;t->device_io=0;t->device_bdf=0;t->device_fault=0;t->device_modern=0;t->cr3=space_new(t->pid);if(!t->cr3)goto fail;
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
 t->frame=(Frame*)(t->kstack+sizeof(t->kstack)-sizeof(Frame));memset(t->frame,0,sizeof(Frame));t->frame->rip=image_base+p->entry;t->frame->cs=0x1b;t->frame->ss=0x23;t->frame->rflags=0x202;t->frame->rsp=SF_STACK-8;t->frame->rdi=SF_ARGS;t->frame->rsi=args->size;t->state=T_READY;t->exit_status=0;return t->pid;
fail: device_release(t);ipc_cleanup(t);pages_free(t->pid);device_forget(t);t->state=T_DEAD;t->exit_status=result;return result;
}
void task_dead(Task*t,int status){device_release(t);ipc_cleanup(t);t->state=T_DEAD;t->exit_status=status;if(current>=0&&t==&tasks[current])__asm__ volatile("mov %0,%%cr3"::"r"(boot_cr3):"memory");pages_free(t->pid);device_forget(t);}
Frame *schedule(void){
 for(;;){for(int n=1;n<=SF_MAX_TASKS;n++){int i=(current+n)%SF_MAX_TASKS;if(tasks[i].state==T_READY){
   Task*t=&tasks[i];uint64_t rsp=t->frame->rsp;
   /* An arbitrary ring-3 RSP can make IRET fault in ring 0. Validate before
    * returning, independently of the emulator's IRET canonicality checks. */
   if(rsp>=0x0000800000000000ull&&rsp<0xffff800000000000ull){if(t->pid==1)panic("initial task return frame");task_dead(t,-113);continue;}
   current=i;arch_task(t);return t->frame;}}
  __asm__ volatile("sti; hlt; cli");
 }
}
static long syscall(Frame*f){Task*t=&tasks[current];uint64_t a=f->rdi,b=f->rsi,c=f->rdx;
 switch(f->rax){
 case SYS_EXIT:if(t->pid==1)panic("initial task exited");task_dead(t,(int)a);return 0;
 case SYS_YIELD:return 0;
 case SYS_LOG:{if(b>512||!user_range(t,a,b,0))return SF_DENIED;char buf[513];copy_in(t,buf,a,b);buf[b]=0;serial(buf);return b;}
 case SYS_TICKS:return ticks;
 case SYS_SPAWN:{if(t->pid!=1)return SF_DENIED;SfLoadPlan plan;if(copy_in(t,&plan,a,sizeof(plan)))return SF_DENIED;return task_new(&plan,0);}
 case SYS_KILL:if(t->pid!=1||a==1)return SF_DENIED;for(unsigned i=0;i<SF_MAX_TASKS;i++)if(tasks[i].pid==a&&tasks[i].state!=T_DEAD){task_dead(&tasks[i],(int)b);return 0;}return SF_NOT_FOUND;
 case SYS_TASK_STATUS:if(t->pid!=1)return SF_DENIED;for(unsigned i=0;i<SF_MAX_TASKS;i++)if(tasks[i].pid==a&&tasks[i].state!=T_EMPTY)return tasks[i].state==T_DEAD?(long)tasks[i].exit_status:SF_TASK_RUNNING;return SF_NOT_FOUND;
 case SYS_CONSOLE_READ:if(t->pid!=1)return SF_DENIED;return console_read();
 case SYS_SHUTDOWN:if(t->pid!=1)return SF_DENIED;__asm__ volatile("outl %0,%1"::"a"((uint32_t)a),"Nd"((uint16_t)0xf4));outb(0x604,0);return 0;
 case SYS_MAP_TEST:return SF_DENIED;
 case SYS_PCI_READ:if(t->pid!=1)return SF_DENIED;return pci_read(a,b);
 case SYS_PORT_IO:return device_io(t,a,b,c);
 case SYS_DEVICE_RELEASE:if(!a)return device_release(t);if(t->pid!=1)return SF_DENIED;for(unsigned i=0;i<SF_MAX_TASKS;i++)if(tasks[i].pid==a)return device_release(&tasks[i]);return SF_NOT_FOUND;
 case SYS_DMA_STATUS:{if(t->pid!=1)return SF_DENIED;if(b!=sizeof(SfDmaStatus))return SF_INVALID;SfDmaStatus status;device_status(&status);return copy_out(t,a,&status,sizeof(status));}
 default:return ipc_syscall(f->rax,a,b,c);
 }
}
Frame *interrupt_dispatch(Frame*f){
 if(f->vector==32){ticks++;outb(0x20,0x20);ipc_deadlines();}
 if(f->vector==36)console_irq();
 if((f->cs&3)==0){if(f->vector<32){serial("[CORE] kernel exception ");hex(f->vector);serial(" at ");hex(f->rip);panic("kernel exception");}return f;}
 Task*t=&tasks[current];t->frame=f;device_poll();if(t->state==T_DEAD)return schedule();
 if(f->vector==128)f->rax=syscall(f);
 else if(f->vector<32){serial("[CORE] isolated user fault pid=");hex(t->pid);serial(" vector=");hex(f->vector);serial("\n");if(t->pid==1)panic("initial task fault");task_dead(t,-100-(int)f->vector);}
 return schedule();
}
void kernel_main(SfBootInfo*b){
 __asm__ volatile("mov %%cr3,%0":"=r"(boot_cr3));
 serial("\nSpiritFoxOS Core 0 / ABI 1\n");
 if((uint64_t)b>=0x40000000ull||b->magic!=0x5f1f0f05||b->boot_type!=1||b->size!=sizeof(*b)||b->version!=1||!b->init_segment_count||b->init_segment_count>8)panic("BootInfo rejected");
 if((b->bundle&4095)||!b->bundle_bytes||b->bundle_bytes>4*1024*1024||b->bundle>=0x40000000ull||b->bundle_bytes>0x40000000ull-b->bundle)panic("bundle bounds");
 if(b->memory_map_descriptor_size!=sizeof(BootMemory)||!b->memory_map_entry_count||b->memory_map_entry_count>2048||b->memory_map_size!=b->memory_map_entry_count*sizeof(BootMemory)||b->memory_map>=0x40000000ull||b->memory_map_size>0x40000000ull-b->memory_map)panic("memory map bounds");
 if(!boot_memory(b,(uint64_t)b,sizeof(*b))||!boot_memory(b,b->bundle,b->bundle_bytes))panic("unreserved boot object");
 bundle_phys=b->bundle;bundle_bytes=b->bundle_bytes;arch_init();device_init(b);SfLoadPlan p={0};p.size=sizeof(p);p.count=b->init_segment_count;p.entry=b->init_entry;
 for(unsigned i=0;i<p.count;i++){SfBootSegment*s=&b->init_segments[i];if(s->reserved||s->file_bytes>s->memory_bytes||(s->physical&4095)||!boot_memory(b,s->physical,(s->memory_bytes+4095)&~4095ull))panic("boot segment");if(s->physical<b->bundle+b->bundle_bytes&&b->bundle<s->physical+s->memory_bytes)panic("boot segment overlaps bundle");for(unsigned j=0;j<i;j++){SfBootSegment*t=&b->init_segments[j];if(s->physical<t->physical+t->memory_bytes&&t->physical<s->physical+s->memory_bytes)panic("overlapping boot segments");}p.segments[i]=(SfLoadSegment){s->rva,s->memory_bytes,s->physical,s->flags,0};}
 if(task_new(&p,1)!=1)panic("bootstrap plan rejected");serial("[CORE] entering isolated sf-init.sfmod, timer=100Hz\n");arch_enter(schedule());
}
