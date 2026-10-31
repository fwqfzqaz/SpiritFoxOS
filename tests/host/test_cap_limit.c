#include "core_fixture.h"
static long limit(uint64_t pid,uint64_t budget,uint64_t op){Frame f={0};f.rax=SYS_TASK_CAP_LIMIT;f.rdi=pid;f.rsi=budget;f.rdx=op;return syscall(&f);}
int main(void){
 reset(10);tasks[1].pid=2;tasks[1].state=T_BLOCKED;tasks[1].cap_limit=32;tasks[1].priority=3;tasks[1].ready_since=7;
 CHECK(limit(2,0,SF_LIMIT_QUERY)==32&&limit(2,0,SF_LIMIT_SET)==0&&limit(2,0,SF_LIMIT_QUERY)==0);
 CHECK(tasks[1].state==T_BLOCKED&&tasks[1].priority==3&&tasks[1].ready_since==7);
 CHECK(limit(2,32,SF_LIMIT_SET)==32);
 const uint64_t bad_pid[]={0,1ull<<32|2,1ull<<63|2,0x80000000ull,UINT64_MAX};
 for(unsigned i=0;i<sizeof(bad_pid)/sizeof(*bad_pid);i++){memcpy(saved,tasks,sizeof(tasks));CHECK(limit(bad_pid[i],0,SF_LIMIT_SET)==SF_INVALID&&!memcmp(saved,tasks,sizeof(tasks)));}
 memcpy(saved,tasks,sizeof(tasks));CHECK(limit(2,33,SF_LIMIT_SET)==SF_INVALID&&limit(2,1ull<<32|2,SF_LIMIT_SET)==SF_INVALID&&!memcmp(saved,tasks,sizeof(tasks)));
 CHECK(limit(2,1,SF_LIMIT_QUERY)==SF_INVALID&&limit(2,0,2)==SF_INVALID&&limit(2,0,1ull<<32|SF_LIMIT_SET)==SF_INVALID&&!memcmp(saved,tasks,sizeof(tasks)));
 CHECK(limit(INT32_MAX,0,SF_LIMIT_QUERY)==SF_NOT_FOUND&&!memcmp(saved,tasks,sizeof(tasks)));
 current=1;CHECK(limit(2,32,SF_LIMIT_SET)==SF_DENIED&&limit(2,0,SF_LIMIT_QUERY)==SF_DENIED&&limit(UINT64_MAX,UINT64_MAX,UINT64_MAX)==SF_DENIED&&!memcmp(saved,tasks,sizeof(tasks)));current=0;
 uint32_t seed=0x7acf18u;
 for(unsigned trial=0;trial<1000;trial++){
  reset(100);tasks[1].pid=2;tasks[1].state=trial&1?T_BLOCKED:T_READY;tasks[1].cap_limit=32;tasks[1].priority=3;tasks[1].ready_since=1234;
  seed=1664525u*seed+1013904223u;uint32_t mask=seed;unsigned used=0;
  for(unsigned i=0;i<32;i++){if(mask&(1u<<i)){tasks[1].caps[i]=(Cap){i+1,7,i&7,91+i};used++;}}
  seed=1664525u*seed+1013904223u;unsigned requested=seed%33;memcpy(saved,tasks,sizeof(tasks));
  long r=limit(2,requested,SF_LIMIT_SET);CHECK(r==(used>requested?SF_BUSY:(long)requested));
  if(used<=requested)saved[1].cap_limit=requested;
  CHECK(!memcmp(saved,tasks,sizeof(tasks))&&task_caps_used(&tasks[1])==used);
  CHECK(limit(2,0,SF_LIMIT_QUERY)==(used>requested?32:(long)requested));
  CHECK(limit(2,32,SF_LIMIT_SET)==32&&tasks[1].state==(trial&1?T_BLOCKED:T_READY)&&tasks[1].ready_since==1234);
 }
 reset(42);tasks[0].cap_limit=0;tasks[1].state=T_DEAD;tasks[1].cap_limit=1;SfLoadPlan p=plan();
 CHECK(task_new(&p,0)==42&&tasks[1].cap_limit==32&&tasks[0].cap_limit==0);
 tasks[1].state=T_DEAD;memcpy(saved,tasks,sizeof(tasks));CHECK(limit(42,32,SF_LIMIT_SET)==SF_NOT_FOUND&&!memcmp(saved,tasks,sizeof(tasks)));
 printf("CAP LIMIT PASS: %u checks; actual root control, 1000 independent live-bit masks, lowering busy/no mutation, width/authority/zero limit and default on slot reuse\n",checks);return 0;
}
