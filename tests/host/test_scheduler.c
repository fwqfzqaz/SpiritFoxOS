#include "core_fixture.h"
static Frame frames[SF_MAX_TASKS];
static void ready(unsigned count){
 reset(100);ticks=0;core_allow_schedule=1;core_last_arch=0;
 for(unsigned i=0;i<count;i++){tasks[i].pid=i+1;tasks[i].state=T_READY;tasks[i].priority=SF_PRIORITY_DEFAULT;
  memset(&frames[i],0,sizeof(frames[i]));frames[i].rsp=SF_STACK-8;tasks[i].frame=&frames[i];}
 current=(int)count-1;
}
static long control(uint64_t pid,uint64_t level,uint64_t op){Frame f={0};f.rax=SYS_TASK_PRIORITY;f.rdi=pid;f.rsi=level;f.rdx=op;return syscall(&f);}
int main(void){
 SfSchedEntry entries[SF_MAX_TASKS]={0},copy[SF_MAX_TASKS];
 CHECK(sf_sched_pick(0,1,-1,0)==-1&&sf_sched_pick(entries,0,-1,0)==-1);
 CHECK(sf_sched_pick(entries,SF_MAX_TASKS+1,-1,0)==-1&&sf_sched_pick(entries,2,-2,0)==-1&&sf_sched_pick(entries,2,2,0)==-1);
 CHECK(sf_sched_pick(entries,2,-1,0)==-1);
 entries[0]=(SfSchedEntry){0,0,1};entries[1]=(SfSchedEntry){0,3,1};memcpy(copy,entries,sizeof(copy));
 CHECK(sf_sched_pick(entries,2,-1,11)==1&&sf_sched_pick(entries,2,1,12)==0);
 CHECK(sf_sched_pick(entries,2,0,12)==1&&!memcmp(entries,copy,sizeof(copy)));
 entries[0].ready_since=UINT64_MAX;CHECK(sf_sched_pick(entries,2,1,0)==1);
 entries[0].ready_since=0;CHECK(sf_sched_pick(entries,2,1,UINT64_MAX)==0);
 entries[1].priority=4;CHECK(sf_sched_pick(entries,2,-1,0)==0);entries[0].ready=0;CHECK(sf_sched_pick(entries,2,-1,0)==-1);
 /* Actual schedule: original equal-level circular order and higher readiness. */
 ready(4);for(unsigned i=0;i<20;i++){unsigned expected=i%4;CHECK(schedule()==&frames[expected]&&current==(int)expected&&core_last_arch==&tasks[expected]);}
 ready(2);tasks[0].priority=0;tasks[1].priority=3;CHECK(schedule()==&frames[1]&&current==1);
 ticks=11;CHECK(schedule()==&frames[1]);ticks=12;CHECK(schedule()==&frames[0]);CHECK(schedule()==&frames[1]);
 CHECK(tasks[0].ready_since==12&&tasks[1].ready_since==12);
 /* Bad return frame is still rejected before entering the selected task. */
 ready(2);current=0;tasks[1].priority=3;frames[1].rsp=0x800000000000ull;
 CHECK(schedule()==&frames[0]&&tasks[1].state==T_DEAD&&tasks[1].exit_status==-113&&freed==1&&freed_pid==2);
 /* Privileged configuration rejects aliases and never mutates on failure. */
 ready(3);current=0;tasks[1].state=T_BLOCKED;tasks[1].ready_since=19;ticks=100;
 CHECK(control(2,3,SF_PRIORITY_SET)==3&&tasks[1].priority==3&&tasks[1].state==T_BLOCKED&&tasks[1].ready_since==19);
 CHECK(control(2,0,SF_PRIORITY_QUERY)==3&&control(1,0,SF_PRIORITY_QUERY)==1);
 const uint64_t invalid_pid[]={0,1ull<<32|2,1ull<<63|2,UINT64_MAX,0x80000000ull};
 for(unsigned i=0;i<sizeof(invalid_pid)/sizeof(*invalid_pid);i++){memcpy(saved,tasks,sizeof(tasks));CHECK(control(invalid_pid[i],0,SF_PRIORITY_SET)==SF_INVALID&&!memcmp(saved,tasks,sizeof(tasks)));}
 memcpy(saved,tasks,sizeof(tasks));CHECK(control(2,4,SF_PRIORITY_SET)==SF_INVALID&&control(2,1ull<<32|1,SF_PRIORITY_SET)==SF_INVALID&&!memcmp(saved,tasks,sizeof(tasks)));
 CHECK(control(2,1,SF_PRIORITY_QUERY)==SF_INVALID&&control(2,0,2)==SF_INVALID&&control(2,0,1ull<<32|SF_PRIORITY_SET)==SF_INVALID&&!memcmp(saved,tasks,sizeof(tasks)));
 CHECK(control(99,0,SF_PRIORITY_QUERY)==SF_NOT_FOUND&&!memcmp(saved,tasks,sizeof(tasks)));
 tasks[1].state=T_DEAD;memcpy(saved,tasks,sizeof(tasks));CHECK(control(2,0,SF_PRIORITY_SET)==SF_NOT_FOUND&&!memcmp(saved,tasks,sizeof(tasks)));
 current=2;memcpy(saved,tasks,sizeof(tasks));CHECK(control(3,3,SF_PRIORITY_SET)==SF_DENIED&&control(3,0,SF_PRIORITY_QUERY)==SF_DENIED&&control(UINT64_MAX,UINT64_MAX,UINT64_MAX)==SF_DENIED&&!memcmp(saved,tasks,sizeof(tasks)));
 /* Slot reuse discards the prior task's priority and waiting timestamp. */
 reset(7);ticks=123;tasks[1].state=T_DEAD;tasks[1].priority=3;tasks[1].ready_since=UINT64_MAX;SfLoadPlan p=plan();
 CHECK(task_new(&p,0)==7&&tasks[1].priority==SF_PRIORITY_DEFAULT&&tasks[1].ready_since==123);
 reset(9);ticks=77;core_map_tick_step=1;CHECK(task_new(&p,0)==9&&ticks>77&&tasks[1].ready_since==ticks);
 /* Selection bound, with all 2..12 tasks continuously ready. This advances
  * one timer tick per actual dispatch, independently of syscall traffic. */
 uint32_t seed=0x591ac7u;unsigned longest=0;
 for(unsigned scenario=0;scenario<1000;scenario++){
  seed=1664525u*seed+1013904223u;unsigned count=2+seed%(SF_MAX_TASKS-1);ready(count);
  uint64_t last[SF_MAX_TASKS]={0};unsigned runs[SF_MAX_TASKS]={0};
  for(unsigned i=0;i<count;i++){seed=1664525u*seed+1013904223u;tasks[i].priority=seed%4;}
  for(unsigned step=1;step<=200;step++){
   ticks=step;Frame*f=schedule();CHECK(current>=0&&(unsigned)current<count&&f==&frames[current]&&core_last_arch==&tasks[current]);
   runs[current]++;last[current]=ticks;
   for(unsigned i=0;i<count;i++){uint64_t wait=ticks-last[i];if(wait>longest)longest=(unsigned)wait;CHECK(wait<=SF_PRIORITY_AGING_TICKS*SF_PRIORITY_MAX+count+1);}
  }
  for(unsigned i=0;i<count;i++)CHECK(runs[i]>0);
 }
 printf("SCHEDULER PASS: %u checks; actual selection/configuration, 1000 ready-set traces, maximum observed dispatch gap %u ticks, aliases/authority/reuse and bad return frame\n",checks,longest);return 0;
}
