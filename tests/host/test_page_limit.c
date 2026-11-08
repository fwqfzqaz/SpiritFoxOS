#include "core_fixture.h"
static long page_limit(uint64_t pid,uint64_t pages,uint64_t op){Frame f={0};f.rax=SYS_TASK_PAGE_LIMIT;f.rdi=pid;f.rsi=pages;f.rdx=op;return syscall(&f);}
static long spawn_limited(SfLoadPlan*p,uint64_t pages,uint64_t reserved){Frame f={0};f.rax=SYS_SPAWN_LIMITED;f.rdi=(uintptr_t)p;f.rsi=pages;f.rdx=reserved;return syscall(&f);}
int main(void){
 reset(42);tasks[1].pid=2;tasks[1].state=T_BLOCKED;tasks[1].main=1;tasks[1].main_slot=1;tasks[1].page_limit=4096;tasks[1].page_count=7;
 memcpy(saved,tasks,sizeof(tasks));CHECK(page_limit(2,0,SF_LIMIT_USED)==7&&page_limit(2,0,SF_LIMIT_QUERY)==4096&&!memcmp(saved,tasks,sizeof(tasks)));
 CHECK(page_limit(2,6,SF_LIMIT_SET)==SF_BUSY&&!memcmp(saved,tasks,sizeof(tasks)));
 CHECK(page_limit(2,7,SF_LIMIT_SET)==7&&tasks[1].page_count==7&&tasks[1].state==T_BLOCKED);
 memcpy(saved,tasks,sizeof(tasks));uint64_t invalid[]={0,INT32_MAX+1ull,1ull<<32|2,UINT64_MAX};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++)CHECK(page_limit(invalid[i],0,SF_LIMIT_USED)==SF_INVALID&&!memcmp(saved,tasks,sizeof(tasks)));
 CHECK(page_limit(2,1,SF_LIMIT_USED)==SF_INVALID&&page_limit(2,1,SF_LIMIT_QUERY)==SF_INVALID&&page_limit(2,4097,SF_LIMIT_SET)==SF_INVALID&&page_limit(2,1ull<<32|7,SF_LIMIT_SET)==SF_INVALID&&!memcmp(saved,tasks,sizeof(tasks)));
 CHECK(page_limit(2,0,3)==SF_INVALID&&page_limit(2,0,1ull<<32|SF_LIMIT_USED)==SF_INVALID&&!memcmp(saved,tasks,sizeof(tasks)));
 CHECK(page_limit(INT32_MAX,0,SF_LIMIT_QUERY)==SF_NOT_FOUND);
 current=1;CHECK(page_limit(2,0,SF_LIMIT_USED)==SF_DENIED&&page_limit(2,4096,SF_LIMIT_SET)==SF_DENIED&&page_limit(UINT64_MAX,UINT64_MAX,UINT64_MAX)==SF_DENIED&&spawn_limited(0,UINT64_MAX,UINT64_MAX)==SF_DENIED&&!memcmp(saved,tasks,sizeof(tasks)));current=0;
 uint32_t seed=0xad49u;
 for(unsigned trial=0;trial<1000;trial++){
  reset(42);tasks[1].pid=2;tasks[1].state=trial&1?T_READY:T_BLOCKED;tasks[1].main=1;tasks[1].main_slot=1;tasks[1].page_limit=4096;
  seed=seed*1664525u+1013904223u;unsigned used=trial==0?0:trial==1?4096:seed%4097;tasks[1].page_count=used;
  seed=seed*1664525u+1013904223u;unsigned requested=seed%4097;memcpy(saved,tasks,sizeof(tasks));
  CHECK(page_limit(2,0,SF_LIMIT_USED)==(long)used&&!memcmp(saved,tasks,sizeof(tasks)));
  CHECK(page_limit(2,requested,SF_LIMIT_SET)==(used>requested?SF_BUSY:(long)requested));
  if(used<=requested)saved[1].page_limit=requested;
  CHECK(!memcmp(saved,tasks,sizeof(tasks))&&page_limit(2,0,SF_LIMIT_USED)==(long)used);
 }
 reset(42);SfLoadPlan p=plan();memcpy(saved,tasks,sizeof(tasks));
 CHECK(spawn_limited(&p,4097,0)==SF_INVALID&&spawn_limited(&p,1ull<<32|64,0)==SF_INVALID&&spawn_limited(&p,64,1)==SF_INVALID&&!memcmp(saved,tasks,sizeof(tasks))&&!allocation_calls&&next_pid==42);
 for(unsigned budget=0;budget<18;budget++){
  reset(42);CHECK(spawn_limited(&p,budget,0)==SF_NO_SPACE&&next_pid==43&&tasks[1].state==T_DEAD&&!tasks[1].page_count&&freed==1&&freed_pid==42);
 }
 reset(42);CHECK(spawn_limited(&p,18,0)==42&&tasks[1].page_count==18&&tasks[1].page_limit==18);
 tasks[1].state=T_DEAD;pages_free(42);tasks[0].page_limit=0;
 CHECK(task_new(&p,0)==43&&tasks[1].page_limit==SF_PAGE_LIMIT_MAX&&tasks[1].page_count==18&&tasks[0].page_limit==0);
 tasks[1].state=T_DEAD;memcpy(saved,tasks,sizeof(tasks));CHECK(page_limit(43,0,SF_LIMIT_USED)==SF_NOT_FOUND&&!memcmp(saved,tasks,sizeof(tasks)));
 printf("PAGE LIMIT PASS: %u checks; actual control/limited spawn, 1000 independent use/budget pairs, authority/width/busy, preallocation policy, rollback and default on reuse\n",checks);return 0;
}
