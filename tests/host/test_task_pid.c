#include "core_fixture.h"
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
