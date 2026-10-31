#ifndef SF_HOST_CORE_FIXTURE_H
#define SF_HOST_CORE_FIXTURE_H
#include <stdio.h>
#include <stdlib.h>
/* LLP64 host: unused TASK_STATUS branch has a guest LP64 sentinel; only creation paths run here. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconstant-conversion"
#include "../../kernel/core.c"
#pragma clang diagnostic pop
static uint8_t mock_pages[64][4096] __attribute__((aligned(4096))),segment[4096];
static Task saved[SF_MAX_TASKS];static unsigned allocated,allocation_calls,space_calls,fail_after=64,freed,checks;
static uint32_t freed_pid;
static int core_allow_schedule;static Task*core_last_arch;static uint64_t core_map_tick_step;
#define CHECK(x) do{checks++;if(!(x)){printf("FAIL line %d: %s (next=%u, slot1_pid=%u, state=%u, allocations=%u)\n",__LINE__,#x,next_pid,tasks[1].pid,tasks[1].state,allocation_calls);return 1;}}while(0)
void serial(const char*s){(void)s;}void hex(uint64_t x){(void)x;}void panic(const char*s){fprintf(stderr,"Unexpected panic: %s\n",s);abort();}
void console_irq(void){}long console_read(void){return SF_WOULD_BLOCK;}void arch_init(void){}void arch_task(Task*t){if(core_allow_schedule){core_last_arch=t;return;}abort();}void arch_save(Task*t){(void)t;abort();}void arch_enter(Frame*f){(void)f;abort();}
void*page_alloc(uint32_t owner){(void)owner;allocation_calls++;if(allocated>=fail_after||allocated>=64)return 0;void*p=mock_pages[allocated++];memset(p,0,4096);return p;}
void pages_free(uint32_t owner){freed++;freed_pid=owner;}uint64_t space_new(uint32_t owner){(void)owner;space_calls++;return 4096;}
int space_map(Task*t,uint64_t a,uint64_t b,uint32_t flags){(void)t;(void)a;(void)b;(void)flags;ticks+=core_map_tick_step;return 0;}
int user_range(Task*t,uint64_t a,uint64_t n,int write){(void)t;(void)a;(void)n;(void)write;return 1;}
int copy_in(Task*t,void*d,uint64_t s,size_t n){(void)t;memcpy(d,(void*)(uintptr_t)s,n);return 0;}
int copy_out(Task*t,uint64_t d,const void*s,size_t n){(void)t;memcpy((void*)(uintptr_t)d,s,n);return 0;}
long ipc_syscall(uint64_t a,uint64_t b,uint64_t c,uint64_t d){(void)a;(void)b;(void)c;(void)d;abort();}
void ipc_cleanup(Task*t){(void)t;}int ipc_grant(Task*a,Task*b,uint32_t c,uint32_t d){(void)a;(void)b;(void)c;(void)d;return 1;}void ipc_deadlines(void){}
long pci_read(uint64_t a,uint64_t b){(void)a;(void)b;return SF_DENIED;}long device_io(Task*t,uint64_t a,uint64_t b,uint64_t c){(void)t;(void)a;(void)b;(void)c;return SF_DENIED;}
int device_grant(Task*t,uint32_t a,uint32_t b,SfBootArgs*c){(void)t;(void)a;(void)b;(void)c;return SF_DENIED;}int device_release(Task*t){(void)t;return 0;}void device_forget(Task*t){(void)t;}
void device_init(SfBootInfo*b){(void)b;}void device_poll(void){}int device_status(SfDmaStatus*s){memset(s,0,sizeof(*s));return 0;}
uint8_t kernel_end[1];
static void reset(uint32_t next){memset(tasks,0,sizeof(tasks));tasks[0].pid=1;tasks[0].state=T_READY;current=0;next_pid=next;allocated=allocation_calls=space_calls=freed=0;freed_pid=0;fail_after=64;core_map_tick_step=0;core_allow_schedule=0;core_last_arch=0;}
static SfLoadPlan plan(void){SfLoadPlan p={0};p.size=sizeof(p);p.count=1;p.image_base=SF_BASE;p.segments[0]=(SfLoadSegment){0,4096,(uint64_t)(uintptr_t)segment,5,0};return p;}
#endif
