#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/ipc.c"
Task tasks[SF_MAX_TASKS];int current;uint64_t ticks;
static Frame frames[SF_MAX_TASKS];static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"SCHED IPC FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int copy_in(Task*t,void*out,uint64_t from,size_t n){(void)t;memcpy(out,(void*)(uintptr_t)from,n);return 0;}
int copy_out(Task*t,uint64_t to,const void*in,size_t n){(void)t;memcpy((void*)(uintptr_t)to,in,n);return 0;}
int user_range(Task*t,uint64_t a,uint64_t n,int write){(void)t;(void)a;(void)n;(void)write;return 1;}
void panic(const char*s){fprintf(stderr,"Unexpected panic: %s\n",s);abort();}
static void setup(void){
 memset(tasks,0,sizeof(tasks));memset(endpoints,0,sizeof(endpoints));memset(frames,0,sizeof(frames));
 for(unsigned i=0;i<2;i++){tasks[i].pid=i+1;tasks[i].state=T_READY;tasks[i].cap_limit=SF_CAP_LIMIT_MAX;tasks[i].frame=&frames[i];tasks[i].priority=i?0:3;tasks[i].ready_since=1;}
 current=0;
}
int main(void){
 uint32_t seed=0x178ca3u;
 for(unsigned scenario=0;scenario<1000;scenario++){
  setup();seed=1664525u*seed+1013904223u;ticks=2+seed%100000;
  long sender=ipc_syscall(SYS_EP_CREATE,0,0,0);CHECK(sender>0);int recv=ipc_grant(&tasks[0],&tasks[1],sender,RIGHT_RECV);CHECK(recv>0);
  SfMessage input={0},output;input.h.size=40;input.h.protocol_major=1;input.h.request_id=scenario+1;
  current=1;CHECK(ipc_syscall(SYS_RECV,recv,(uintptr_t)&output,0)==0&&tasks[1].state==T_BLOCKED);uint64_t since=tasks[1].ready_since;
  ticks+=15;current=0;CHECK(!ipc_syscall(SYS_SEND,sender,(uintptr_t)&input,0));
  CHECK(tasks[1].state==T_READY&&tasks[1].ready_since==ticks&&tasks[1].ready_since>since&&tasks[1].priority==0&&frames[1].rax==1&&output.h.request_id==scenario+1);
  current=1;CHECK(!ipc_syscall(SYS_RECV,recv,(uintptr_t)&output,(ticks+2)*10000000ull)&&tasks[1].state==T_BLOCKED);
  ticks+=2;ipc_deadlines();CHECK(tasks[1].state==T_READY&&tasks[1].ready_since==ticks&&tasks[1].priority==0&&(int64_t)frames[1].rax==SF_TIMEOUT);
  CHECK(!ipc_syscall(SYS_RECV,recv,(uintptr_t)&output,0)&&tasks[1].state==T_BLOCKED);
  ticks+=17;current=0;CHECK(!ipc_syscall(SYS_CAP_REVOKE,sender,0,0));
  CHECK(tasks[1].state==T_READY&&tasks[1].ready_since==ticks&&tasks[1].priority==0&&(int64_t)frames[1].rax==SF_REVOKED);
 }
 printf("SCHED IPC PASS: %u checks; 1000 actual send/deadline/revocation wakeups start fresh ready intervals without changing base priority\n",checks);return 0;
}
