#ifndef SF_HOST_CORE_FIXTURE_H
#define SF_HOST_CORE_FIXTURE_H
#include <stdio.h>
#include <stdlib.h>
/* LLP64 host: unused TASK_STATUS branch has a guest LP64 sentinel; only creation paths run here. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconstant-conversion"
#include "../../kernel/core.c"
#pragma clang diagnostic pop
static uint8_t mock_pages[256][4096] __attribute__((aligned(4096))),segment[4096];
static Task saved[SF_MAX_THREADS];static unsigned allocated,allocation_calls,space_calls,fail_after=256,freed,checks;
static uint32_t freed_pid;
static int core_allow_schedule;static Task*core_last_arch;static uint64_t core_map_tick_step;
static int core_copy_out_denied;static unsigned core_copy_out_calls;
static int core_map_deny;static unsigned core_unmap_calls;static uint64_t core_unmap_bytes;static unsigned core_page_free_calls;
#define CHECK(x) do{checks++;if(!(x)){printf("FAIL line %d: %s (next=%u, slot1_pid=%u, state=%u, allocations=%u)\n",__LINE__,#x,next_pid,tasks[1].pid,tasks[1].state,allocation_calls);return 1;}}while(0)
void serial(const char*s){(void)s;}void hex(uint64_t x){(void)x;}void panic(const char*s){fprintf(stderr,"Unexpected panic: %s\n",s);abort();}
void console_irq(void){}long console_read(void){return SF_WOULD_BLOCK;}void ps2_mouse_init(uint32_t width,uint32_t height){(void)width;(void)height;}long ps2_mouse_read(int reset){(void)reset;return SF_WOULD_BLOCK;}void ps2_irq(void){}void arch_init(void){}void arch_task(Task*t){if(core_allow_schedule){core_last_arch=t;return;}abort();}void arch_save(Task*t){(void)t;abort();}void arch_enter(Frame*f){(void)f;abort();}
void*page_alloc(uint32_t owner){
 allocation_calls++;if(allocated>=fail_after||allocated>=256)return 0;Task*t=0;
 for(unsigned i=0;i<SF_MAX_THREADS;i++)if(tasks[i].main&&tasks[i].pid==owner){t=&tasks[i];break;}
 if(!t||t->page_count>=t->page_limit)return 0;t->page_count++;
 void*p=mock_pages[allocated++];memset(p,0,4096);return p;
}
void page_free_phys(uint32_t owner,uint64_t physical){(void)physical;core_page_free_calls++;for(unsigned i=0;i<SF_MAX_THREADS;i++)if(tasks[i].main&&tasks[i].pid==owner&&tasks[i].page_count)tasks[i].page_count--;}
void pages_free(uint32_t owner){freed++;freed_pid=owner;for(unsigned i=0;i<SF_MAX_THREADS;i++)if(tasks[i].pid==owner)tasks[i].page_count=0;}uint64_t space_new(uint32_t owner){(void)owner;space_calls++;return 4096;}
int space_map(Task*t,uint64_t a,uint64_t b,uint32_t flags){(void)t;(void)a;(void)b;(void)flags;ticks+=core_map_tick_step;return core_map_deny?SF_NO_SPACE:0;}
int space_unmap_range(Task*t,uint64_t v,uint64_t n){(void)v;core_unmap_calls++;core_unmap_bytes+=n;Task*m=t->main?t:&tasks[t->main_slot];uint64_t pages=n/4096;if(pages>m->page_count)pages=m->page_count;m->page_count-=(uint32_t)pages;return 0;}
int user_exec(Task*t,uint64_t v){(void)t;return v>=SF_BASE&&v<SF_BASE+0x1000;}
int user_range(Task*t,uint64_t a,uint64_t n,int write){(void)t;(void)a;(void)n;(void)write;return 1;}
/* Core-only tests do not model framebuffer mappings. Fail if a test starts
 * using those paths; actual mappings are exercised by the GUI VM tests. */
void*user_phys(Task*t,uint64_t v){(void)t;(void)v;abort();}
int space_map_fb(Task*t,uint64_t v,uint64_t physical,uint64_t bytes){(void)t;(void)v;(void)physical;(void)bytes;abort();}
int copy_in(Task*t,void*d,uint64_t s,size_t n){(void)t;memcpy(d,(void*)(uintptr_t)s,n);return 0;}
int copy_out(Task*t,uint64_t d,const void*s,size_t n){(void)t;core_copy_out_calls++;if(core_copy_out_denied)return SF_DENIED;memcpy((void*)(uintptr_t)d,s,n);return 0;}
long ipc_syscall(uint64_t a,uint64_t b,uint64_t c,uint64_t d){(void)a;(void)b;(void)c;(void)d;abort();}
void ipc_stats_snapshot(SfIpcStats*s){*s=(SfIpcStats){sizeof(*s),SF_IPC_STATS_VERSION,0x100000001ull,UINT64_MAX-32};}
void ipc_cleanup(Task*t){(void)t;}int ipc_grant(Task*a,Task*b,uint32_t c,uint32_t d){(void)a;(void)b;(void)c;(void)d;return 1;}void ipc_deadlines(void){}
long pci_read(uint64_t a,uint64_t b){(void)a;(void)b;return SF_DENIED;}long device_io(Task*t,uint64_t a,uint64_t b,uint64_t c){(void)t;(void)a;(void)b;(void)c;return SF_DENIED;}
int device_grant(Task*t,uint32_t a,uint32_t b,SfBootArgs*c){(void)t;(void)a;(void)b;(void)c;return SF_DENIED;}int device_release(Task*t){(void)t;return 0;}void device_forget(Task*t){(void)t;}
void device_init(SfBootInfo*b){(void)b;}void device_poll(void){}int device_status(SfDmaStatus*s){memset(s,0,sizeof(*s));return 0;}
void device_irq_init(void){}int device_irq_acquire(Task*t){(void)t;return SF_DENIED;}int device_irq_release(Task*t){(void)t;return SF_DENIED;}
int device_irq_wait(Task*t,uint32_t n){(void)t;(void)n;return SF_DENIED;}void device_irq_vector(void){}
uint8_t kernel_end[1];
static void reset(uint32_t next){memset(tasks,0,sizeof(tasks));tasks[0].pid=1;tasks[0].state=T_READY;tasks[0].main=1;tasks[0].main_slot=0;tasks[0].tid=1;tasks[0].thread_limit=SF_THREAD_LIMIT_MAX;tasks[0].thread_count=1;tasks[0].thread_slots=1;tasks[0].next_tid=1;tasks[0].cpu=-1;tasks[0].cap_limit=SF_CAP_LIMIT_MAX;tasks[0].page_limit=SF_PAGE_LIMIT_MAX;current=0;next_pid=next;allocated=allocation_calls=space_calls=freed=0;freed_pid=0;fail_after=256;core_map_tick_step=0;core_allow_schedule=0;core_last_arch=0;core_copy_out_denied=0;core_copy_out_calls=0;core_map_deny=0;core_unmap_calls=0;core_unmap_bytes=0;core_page_free_calls=0;}
static SfLoadPlan plan(void){SfLoadPlan p={0};p.size=sizeof(p);p.count=1;p.image_base=SF_BASE;p.segments[0]=(SfLoadSegment){0,4096,(uint64_t)(uintptr_t)segment,5,0};return p;}
#endif
