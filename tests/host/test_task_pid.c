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
#define CHECK(x) do{checks++;if(!(x)){printf("FAIL line %d: %s (next=%u, slot1_pid=%u, state=%u, allocations=%u)\n",__LINE__,#x,next_pid,tasks[1].pid,tasks[1].state,allocation_calls);return 1;}}while(0)
void serial(const char*s){(void)s;}void hex(uint64_t x){(void)x;}void panic(const char*s){fprintf(stderr,"Unexpected panic: %s\n",s);abort();}
void console_irq(void){}long console_read(void){return SF_WOULD_BLOCK;}void arch_init(void){}void arch_task(Task*t){(void)t;abort();}void arch_save(Task*t){(void)t;abort();}void arch_enter(Frame*f){(void)f;abort();}
void*page_alloc(uint32_t owner){(void)owner;allocation_calls++;if(allocated>=fail_after||allocated>=64)return 0;void*p=mock_pages[allocated++];memset(p,0,4096);return p;}
void pages_free(uint32_t owner){freed++;freed_pid=owner;}uint64_t space_new(uint32_t owner){(void)owner;space_calls++;return 4096;}
int space_map(Task*t,uint64_t a,uint64_t b,uint32_t flags){(void)t;(void)a;(void)b;(void)flags;return 0;}
int user_range(Task*t,uint64_t a,uint64_t n,int write){(void)t;(void)a;(void)n;(void)write;return 1;}
int copy_in(Task*t,void*d,uint64_t s,size_t n){(void)t;memcpy(d,(void*)(uintptr_t)s,n);return 0;}
int copy_out(Task*t,uint64_t d,const void*s,size_t n){(void)t;memcpy((void*)(uintptr_t)d,s,n);return 0;}
long ipc_syscall(uint64_t a,uint64_t b,uint64_t c,uint64_t d){(void)a;(void)b;(void)c;(void)d;abort();}
void ipc_cleanup(Task*t){(void)t;}int ipc_grant(Task*a,Task*b,uint32_t c,uint32_t d){(void)a;(void)b;(void)c;(void)d;return 1;}void ipc_deadlines(void){}
long pci_read(uint64_t a,uint64_t b){(void)a;(void)b;return SF_DENIED;}long device_io(Task*t,uint64_t a,uint64_t b,uint64_t c){(void)t;(void)a;(void)b;(void)c;return SF_DENIED;}
int device_grant(Task*t,uint32_t a,uint32_t b,SfBootArgs*c){(void)t;(void)a;(void)b;(void)c;return SF_DENIED;}int device_release(Task*t){(void)t;return 0;}void device_forget(Task*t){(void)t;}
void device_init(SfBootInfo*b){(void)b;}void device_poll(void){}int device_status(SfDmaStatus*s){memset(s,0,sizeof(*s));return 0;}
uint8_t kernel_end[1];
static void reset(uint32_t next){memset(tasks,0,sizeof(tasks));tasks[0].pid=1;tasks[0].state=T_READY;current=0;next_pid=next;allocated=allocation_calls=space_calls=freed=0;freed_pid=0;fail_after=64;}
static SfLoadPlan plan(void){SfLoadPlan p={0};p.size=sizeof(p);p.count=1;p.image_base=SF_BASE;p.segments[0]=(SfLoadSegment){0,4096,(uint64_t)(uintptr_t)segment,5,0};return p;}
int main(void){
 SfLoadPlan p=plan();reset(0x80000000u);memcpy(saved,tasks,sizeof(tasks));
 CHECK(task_new(&p,0)==SF_NO_SPACE);CHECK(!memcmp(saved,tasks,sizeof(tasks)));CHECK(next_pid==0x80000000u&&!allocation_calls&&!space_calls);
 reset(INT32_MAX-1u);CHECK(task_new(&p,0)==INT32_MAX-1);CHECK(task_new(&p,0)==INT32_MAX);CHECK(next_pid==0x80000000u);
 CHECK(tasks[1].state==T_READY&&tasks[1].pid==INT32_MAX-1u&&tasks[2].state==T_READY&&tasks[2].pid==INT32_MAX);
 SfBootArgs*a=(void*)mock_pages[allocated-1];CHECK(a->pid==INT32_MAX);
 memcpy(saved,tasks,sizeof(tasks));unsigned calls=allocation_calls;CHECK(task_new(&p,0)==SF_NO_SPACE);CHECK(!memcmp(saved,tasks,sizeof(tasks)));CHECK(allocation_calls==calls&&next_pid==0x80000000u);
 uint32_t seed=0x19bca57u;
 for(unsigned i=0;i<1000;i++){
  seed=1664525u*seed+1013904223u;uint32_t start=i==0?0:i==1?UINT32_MAX:seed;reset(start);memcpy(saved,tasks,sizeof(tasks));int result=task_new(&p,0);
  if(start&&start<=INT32_MAX){CHECK(result>0&&(uint32_t)result==start);CHECK(next_pid==start+1);CHECK(tasks[1].state==T_READY&&tasks[1].pid==start);a=(void*)mock_pages[allocated-1];CHECK(a->pid==start);}
  else{CHECK(result==SF_NO_SPACE);CHECK(next_pid==start);CHECK(!memcmp(saved,tasks,sizeof(tasks)));CHECK(!allocation_calls&&!space_calls);}
  CHECK(tasks[0].pid==1&&tasks[0].state==T_READY);
 }
 reset(42);fail_after=2;CHECK(task_new(&p,0)==SF_NO_SPACE);CHECK(next_pid==43&&tasks[1].state==T_DEAD&&tasks[1].pid==42);CHECK(freed==1&&freed_pid==42);
 allocated=allocation_calls=0;fail_after=64;CHECK(task_new(&p,0)==43);CHECK(next_pid==44&&tasks[1].state==T_READY&&tasks[1].pid==43);
 reset(100);p.count=0;memcpy(saved,tasks,sizeof(tasks));CHECK(task_new(&p,0)==SF_INVALID);CHECK(next_pid==100&&!allocation_calls&&!space_calls&&!memcmp(saved,tasks,sizeof(tasks)));p=plan();
 reset(100);for(unsigned i=0;i<SF_MAX_TASKS;i++){tasks[i].state=T_READY;tasks[i].pid=i+1;}memcpy(saved,tasks,sizeof(tasks));CHECK(task_new(&p,0)==SF_NO_SPACE);CHECK(next_pid==100&&!allocation_calls&&!space_calls&&!memcmp(saved,tasks,sizeof(tasks)));
 reset(100);tasks[0].pid=2;Frame f={0};f.rax=SYS_SPAWN;f.rdi=(uint64_t)(uintptr_t)&p;memcpy(saved,tasks,sizeof(tasks));CHECK(syscall(&f)==SF_DENIED);CHECK(next_pid==100&&!allocation_calls&&!space_calls&&!memcmp(saved,tasks,sizeof(tasks)));
 for(unsigned i=0;i<1000;i++){
  reset(100+i);tasks[1].state=T_DEAD;memset(tasks[1].fxstate,0xa5,sizeof(tasks[1].fxstate));
  CHECK(task_new(&p,0)==(int)(100+i));
  uint8_t clean[512]={0};clean[0]=0x7f;clean[1]=3;clean[24]=0x80;clean[25]=0x1f;
  CHECK(!memcmp(tasks[1].fxstate,clean,sizeof(clean)));CHECK(!((uintptr_t)tasks[1].fxstate&15));
 }
 printf("TASK PID PASS: %u checks; actual task_new, 1000 seeded PID states and 1000 dirty FP slot reuses; no allocation/publication on exhaustion, fresh aligned FP image and failed allocation never reuses PID\n",checks);return 0;
}
