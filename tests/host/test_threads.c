#include "core_fixture.h"
/* 线程模型与配额：创建/共享地址空间、同级退出、主线程连坐、线程配额、
 * CPU 预算过滤与窗口重置。 */
static long invoke(int task,long number,uint64_t a,uint64_t b,uint64_t c){
 current=task;Frame f={0};f.rax=number;f.rdi=a;f.rsi=b;f.rdx=c;return syscall(&f);
}
static long create(int task,uint64_t entry,uint64_t argument){
 SfThreadRequest r={sizeof(r),0,entry,argument};return invoke(task,SYS_THREAD_CREATE,(uint64_t)(uintptr_t)&r,0,0);
}
static void process(uint32_t slot,uint32_t pid){
 Task*t=&tasks[slot];t->pid=pid;t->state=T_READY;t->main=1;t->main_slot=slot;t->tid=1;t->thread_limit=SF_THREAD_LIMIT_MAX;t->thread_count=1;t->thread_slots=1;t->next_tid=1;t->cpu=-1;t->cr3=0x1000u+slot*0x1000u;t->cap_limit=SF_CAP_LIMIT_MAX;t->page_limit=SF_PAGE_LIMIT_MAX;t->priority=SF_PRIORITY_DEFAULT;
}
int main(void){
 const uint64_t entry=SF_BASE;
 CHECK(plan().count==1&&plan().size==sizeof(SfLoadPlan));
 /* 参数校验：尺寸/标志/保留参数/入口可执行性。 */
 reset(2);tasks[0].cr3=0x2000;tasks[0].page_limit=SF_PAGE_LIMIT_MAX;
 SfThreadRequest bad={0};CHECK(invoke(0,SYS_THREAD_CREATE,(uint64_t)(uintptr_t)&bad,1,0)==SF_INVALID);
 bad=(SfThreadRequest){sizeof(bad),1,entry,0};CHECK(invoke(0,SYS_THREAD_CREATE,(uint64_t)(uintptr_t)&bad,0,0)==SF_INVALID);
 bad=(SfThreadRequest){20,0,entry,0};CHECK(invoke(0,SYS_THREAD_CREATE,(uint64_t)(uintptr_t)&bad,0,0)==SF_INVALID);
 CHECK(invoke(0,SYS_THREAD_CREATE,(uint64_t)(uintptr_t)&bad,0,1)==SF_INVALID);
 CHECK(create(0,0x100000,0)==SF_DENIED&&create(0,SF_BASE+0x2000,0)==SF_DENIED);
 CHECK(tasks[0].thread_count==1&&tasks[0].thread_slots==1&&core_unmap_calls==0);
 /* 七个同级线程：共享 cr3/pid/优先级，独立栈域与内核栈，能力槽为空。 */
 for(unsigned i=0;i<SF_THREAD_LIMIT_MAX-1;i++){
  long tid=create(0,entry,0x100+i);CHECK(tid==2+(long)i);
  unsigned slot=1+i;Task*n=&tasks[slot];
  CHECK(n->state==T_READY&&!n->main&&n->main_slot==0&&n->pid==1&&n->cr3==0x2000);
  CHECK(n->tid==(uint32_t)tid&&n->thread_slot==i+1&&n->cpu==-1&&n->priority==tasks[0].priority);
  CHECK(n->thread_limit==0&&n->thread_count==0&&n->thread_slots==0&&n->next_tid==0&&n->cpu_budget==0&&n->cpu_used==0);
  CHECK(n->frame->rip==entry);CHECK(n->frame->rdi==0x100+i);
  uint64_t want=SF_THREAD_STACK_BASE+(uint64_t)(i+2)*SF_THREAD_STACK_BYTES-8;
  CHECK(n->frame->rsp==want);
  CHECK(n->frame->cs==0x1b&&n->frame->ss==0x23&&n->frame->rflags==0x202);
  CHECK(!n->caps[0].endpoint&&n->cap_limit==0&&n->page_limit==0&&n->page_count==0);
 }
 CHECK(tasks[0].thread_count==SF_THREAD_LIMIT_MAX&&tasks[0].thread_slots==255u&&tasks[0].next_tid==8);
 CHECK(tasks[0].page_count==(SF_THREAD_LIMIT_MAX-1)*16&&allocation_calls==(SF_THREAD_LIMIT_MAX-1)*16);
 CHECK(create(0,entry,0)==SF_NO_SPACE);
 tasks[0].caps[0].endpoint=5;CHECK(task_caps_used(&tasks[1])==1&&task_caps_used(&tasks[0])==1);
 /* 同级线程退出只回收自身栈与槽位；主线程与其他线程继续 READY。 */
 current=1;unsigned before=core_unmap_calls;CHECK(invoke(1,SYS_EXIT,7,0,0)==0);
 CHECK(tasks[0].state==T_READY&&tasks[0].thread_count==SF_THREAD_LIMIT_MAX-1&&tasks[0].thread_slots==253u);
 CHECK(tasks[1].state==T_DEAD&&tasks[1].exit_status==7&&core_unmap_calls==before+1&&core_unmap_bytes==SF_THREAD_STACK_BYTES);
 CHECK(tasks[2].state==T_READY&&tasks[7].state==T_READY);
 current=0;long reused=create(0,entry,0x200);CHECK(reused==9&&tasks[1].state==T_READY&&tasks[1].thread_slot==1);
 CHECK(tasks[1].frame->rip==entry&&tasks[1].frame->rdi==0x200&&tasks[0].thread_count==SF_THREAD_LIMIT_MAX&&tasks[0].thread_slots==255u);
 /* 线程配额 query/set/used、边界、低于已用与权限。 */
 CHECK(invoke(0,SYS_THREAD_LIMIT,1,0,SF_LIMIT_QUERY)==SF_THREAD_LIMIT_MAX);
 CHECK(invoke(0,SYS_THREAD_LIMIT,1,0,SF_LIMIT_USED)==SF_THREAD_LIMIT_MAX);
 CHECK(invoke(0,SYS_THREAD_LIMIT,1,4,SF_LIMIT_SET)==SF_BUSY);
 CHECK(invoke(0,SYS_THREAD_LIMIT,1,7,SF_LIMIT_SET)==SF_BUSY);
 CHECK(invoke(0,SYS_THREAD_LIMIT,1,8,SF_LIMIT_SET)==8&&tasks[0].thread_limit==8);
 current=2;CHECK(invoke(2,SYS_EXIT,0,0,0)==0);current=0;
 CHECK(invoke(0,SYS_THREAD_LIMIT,1,7,SF_LIMIT_SET)==7&&tasks[0].thread_limit==7);
 CHECK(invoke(0,SYS_THREAD_LIMIT,1,0,3)==SF_INVALID&&invoke(0,SYS_THREAD_LIMIT,1,1,SF_LIMIT_QUERY)==SF_INVALID);
 CHECK(invoke(0,SYS_THREAD_LIMIT,1,0,SF_LIMIT_SET)==SF_INVALID&&invoke(0,SYS_THREAD_LIMIT,1,9,SF_LIMIT_SET)==SF_INVALID);
 CHECK(invoke(0,SYS_THREAD_LIMIT,0,0,SF_LIMIT_QUERY)==SF_INVALID&&invoke(0,SYS_THREAD_LIMIT,1ull<<32|1,0,SF_LIMIT_QUERY)==SF_INVALID);
 CHECK(invoke(0,SYS_THREAD_LIMIT,99,0,SF_LIMIT_QUERY)==SF_NOT_FOUND);
 process(1,2);current=1;CHECK(invoke(1,SYS_THREAD_LIMIT,1,0,SF_LIMIT_QUERY)==SF_DENIED);current=0;
 /* 主线程退出：全部同级线程被回收，主线程单次 page 释放，栈逐域归还。 */
 reset(2);tasks[0].cr3=0x2000;process(1,2);
 for(unsigned i=0;i<3;i++)CHECK(create(1,entry,0x300+i)==2+(long)i);
 before=core_unmap_calls;unsigned unmapped=core_unmap_bytes;
 current=1;CHECK(invoke(1,SYS_EXIT,9,0,0)==0);
 CHECK(tasks[1].state==T_DEAD&&tasks[1].exit_status==9&&tasks[1].thread_count==1&&tasks[1].thread_slots==1);
 for(unsigned i=2;i<5;i++)CHECK(tasks[i].state==T_DEAD&&tasks[i].exit_status==9);
 CHECK(core_unmap_calls==before+3&&core_unmap_bytes==unmapped+3*SF_THREAD_STACK_BYTES);
 CHECK(freed==1&&freed_pid==2&&tasks[0].state==T_READY);
 current=0;CHECK(invoke(0,SYS_TASK_STATUS,2,0,0)==9);
 /* SYS_KILL 同样按进程回收；PID 1 与非 root 拒绝。 */
 reset(2);tasks[0].cr3=0x2000;process(1,3);
 for(unsigned i=0;i<2;i++)CHECK(create(1,entry,0x400+i)==2+(long)i);
 CHECK(invoke(0,SYS_KILL,3,4,0)==0&&tasks[1].state==T_DEAD&&tasks[1].exit_status==4);
 CHECK(tasks[2].state==T_DEAD&&tasks[3].state==T_DEAD&&tasks[1].thread_count==1&&tasks[1].thread_slots==1);
 CHECK(core_unmap_calls==2&&freed==1&&freed_pid==3&&invoke(0,SYS_TASK_STATUS,3,0,0)==4);
 CHECK(invoke(0,SYS_KILL,1,0,0)==SF_DENIED&&invoke(0,SYS_KILL,99,0,0)==SF_NOT_FOUND);
 current=1;CHECK(invoke(1,SYS_KILL,1,0,0)==SF_DENIED);current=0;
 /* 创建失败回滚：映射被拒绝时归还已分配页并清空占用位。 */
 reset(2);tasks[0].cr3=0x2000;unsigned pages=tasks[0].page_count,slots=tasks[0].thread_slots,count=tasks[0].thread_count;
 core_map_deny=1;CHECK(create(0,entry,0)==SF_NO_SPACE);core_map_deny=0;
 CHECK(tasks[0].thread_count==count&&tasks[0].thread_slots==slots&&tasks[0].page_count==pages);
 CHECK(core_page_free_calls==1&&tasks[1].state==T_DEAD);
 /* CPU 预算：记账只作用于主线程，达到预算后不可调度，窗口重置后恢复。 */
 reset(2);tasks[0].cr3=0x2000;ticks=0;tasks[0].cpu_used=0;
 CHECK(invoke(0,SYS_TASK_CPU_BUDGET,1,0,SF_LIMIT_QUERY)==0&&invoke(0,SYS_TASK_CPU_BUDGET,1,0,SF_LIMIT_USED)==0);
 CHECK(invoke(0,SYS_TASK_CPU_BUDGET,1,50,SF_LIMIT_SET)==50&&tasks[0].cpu_budget==50);
 CHECK(invoke(0,SYS_TASK_CPU_BUDGET,1,101,SF_LIMIT_SET)==SF_INVALID&&invoke(0,SYS_TASK_CPU_BUDGET,1,1,SF_LIMIT_QUERY)==SF_INVALID);
 CHECK(invoke(0,SYS_TASK_CPU_BUDGET,1,0,3)==SF_INVALID&&invoke(0,SYS_TASK_CPU_BUDGET,99,0,SF_LIMIT_QUERY)==SF_NOT_FOUND);
 process(1,4);current=1;CHECK(invoke(1,SYS_TASK_CPU_BUDGET,1,0,SF_LIMIT_QUERY)==SF_DENIED);current=0;
 CHECK(invoke(0,SYS_TASK_CPU_BUDGET,1,50,SF_LIMIT_SET)==50);
 ticks=100;cpu_window_reset();current=0;for(unsigned i=0;i<50;i++)cpu_account();
 CHECK(invoke(0,SYS_TASK_CPU_BUDGET,1,0,SF_LIMIT_USED)==50);
 tasks[0].cpu=-1;tasks[0].state=T_READY;CHECK(!task_schedulable(&tasks[0]));
 CHECK(invoke(0,SYS_TASK_CPU_BUDGET,1,40,SF_LIMIT_SET)==SF_BUSY&&tasks[0].cpu_budget==50);
 CHECK(invoke(0,SYS_TASK_CPU_BUDGET,1,50,SF_LIMIT_SET)==50);
 ticks=200;cpu_window_reset();CHECK(tasks[0].cpu_used==0&&task_schedulable(&tasks[0]));
 CHECK(invoke(0,SYS_TASK_CPU_BUDGET,1,0,SF_LIMIT_SET)==0&&task_schedulable(&tasks[0]));
 /* 同级线程消耗计入同一进程主线程预算。 */
 reset(2);tasks[0].cr3=0x2000;CHECK(create(0,entry,0)==2);
 CHECK(invoke(0,SYS_TASK_CPU_BUDGET,1,3,SF_LIMIT_SET)==3);
 current=1;for(unsigned i=0;i<3;i++)cpu_account();CHECK(tasks[0].cpu_used==3);
 tasks[1].cpu=-1;tasks[1].state=T_READY;CHECK(!task_schedulable(&tasks[1]));
 current=0;ticks=300;cpu_window_reset();CHECK(task_schedulable(&tasks[1]));
 /* 优先级设置传播到该进程全部线程。 */
 reset(2);tasks[0].cr3=0x2000;CHECK(create(0,entry,0)==2);
 CHECK(invoke(0,SYS_TASK_PRIORITY,1,2,SF_PRIORITY_SET)==2&&tasks[0].priority==2);
 CHECK(tasks[0].priority==2&&tasks[1].priority==2);
 printf("THREADS PASS: %u checks; create/share/exit/reuse, process kill, thread limit, failure rollback and CPU budget window filtering\n",checks);return 0;
}
