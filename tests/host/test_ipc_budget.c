#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/ipc.c"
Task tasks[SF_MAX_TASKS];int current;uint64_t ticks;
static Frame frames[SF_MAX_TASKS];static Task saved_tasks[SF_MAX_TASKS];static Endpoint saved_eps[ENDPOINTS];static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"IPC BUDGET FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int copy_in(Task*t,void*out,uint64_t from,size_t n){(void)t;memcpy(out,(void*)(uintptr_t)from,n);return 0;}
int copy_out(Task*t,uint64_t to,const void*in,size_t n){(void)t;memcpy((void*)(uintptr_t)to,in,n);return 0;}
int user_range(Task*t,uint64_t a,uint64_t n,int write){(void)t;(void)a;(void)n;(void)write;return 1;}
void panic(const char*s){fprintf(stderr,"Unexpected panic: %s\n",s);abort();}
static void setup(void){
 memset(tasks,0,sizeof(tasks));memset(endpoints,0,sizeof(endpoints));memset(frames,0,sizeof(frames));
 for(unsigned i=0;i<SF_MAX_TASKS;i++){tasks[i].pid=i+1;tasks[i].state=T_READY;tasks[i].cap_limit=32;tasks[i].frame=&frames[i];}
 current=0;ticks=0;
}
static void snapshot(void){memcpy(saved_tasks,tasks,sizeof(tasks));memcpy(saved_eps,endpoints,sizeof(endpoints));}
static int same(void){return !memcmp(saved_tasks,tasks,sizeof(tasks))&&!memcmp(saved_eps,endpoints,sizeof(endpoints));}
static long create(void){return ipc_syscall(SYS_EP_CREATE,0,0,0);}
static unsigned owners(void){unsigned count=0;for(unsigned i=0;i<ENDPOINTS;i++)if(endpoints[i].owner)count++;return count;}
static void roundtrip(uint32_t h){
 SfMessage in={0},out;in.h.size=40;in.h.protocol_major=1;in.h.request_id=0x109abc;
 CHECK(!ipc_syscall(SYS_SEND,h,(uintptr_t)&in,0));CHECK((int64_t)ipc_syscall(SYS_RECV,h,(uintptr_t)&out,0)==(int64_t)tasks[current].pid&&out.h.request_id==in.h.request_id);
}
int main(void){
 setup();tasks[0].cap_limit=0;snapshot();CHECK(create()==SF_NO_SPACE&&same());
 tasks[0].cap_limit=2;long a=create(),b=ipc_syscall(SYS_CAP_DERIVE,a,RIGHT_SEND,0);CHECK(a>0&&b>0&&task_caps_used(&tasks[0])==2&&owners()==1);
 snapshot();CHECK(create()==SF_NO_SPACE&&same());CHECK(ipc_syscall(SYS_CAP_DERIVE,a,0,0)==SF_NO_SPACE&&same());
 roundtrip(a);CHECK(!ipc_syscall(SYS_CAP_CLOSE,b,0,0));b=create();CHECK(b>0&&owners()==2);
 /* Limits count live handles, without narrowing valid high-index handles. */
 setup();long handles[32];for(unsigned i=0;i<6;i++){handles[i]=create();CHECK(handles[i]>0);}
 for(unsigned i=0;i<5;i++)CHECK(!ipc_syscall(SYS_CAP_CLOSE,handles[i],0,0));
 tasks[0].cap_limit=1;roundtrip(handles[5]);snapshot();CHECK(create()==SF_NO_SPACE&&same());
 CHECK(!ipc_syscall(SYS_CAP_CLOSE,handles[5],0,0)&&!owners());CHECK(create()>0);
 /* Destination limits also cover grants; zero-right aliases still consume budget. */
 setup();a=create();tasks[1].cap_limit=1;long receiver=ipc_grant(&tasks[0],&tasks[1],a,RIGHT_RECV);CHECK(receiver>0);
 snapshot();CHECK(ipc_grant(&tasks[0],&tasks[1],a,0)==SF_NO_SPACE&&same());
 current=1;SfMessage out;CHECK(!ipc_syscall(SYS_RECV,receiver,(uintptr_t)&out,0)&&tasks[1].state==T_BLOCKED);
 current=0;SfMessage in={0};in.h.size=40;in.h.protocol_major=1;CHECK(!ipc_syscall(SYS_SEND,a,(uintptr_t)&in,0)&&tasks[1].state==T_READY);
 current=1;CHECK(!ipc_syscall(SYS_CAP_CLOSE,receiver,0,0)&&!task_caps_used(&tasks[1]));current=0;
 receiver=ipc_grant(&tasks[0],&tasks[1],a,0);CHECK(receiver>0&&task_caps_used(&tasks[1])==1);CHECK(!ipc_syscall(SYS_CAP_REVOKE,a,0,0)&&!task_caps_used(&tasks[1])&&!owners());
 /* Generation retirement and the shared endpoint pool remain separate limits. */
 setup();for(unsigned i=0;i<32;i++)tasks[0].caps[i].generation=0x7fffff;snapshot();CHECK(create()==SF_NO_SPACE&&same());
 current=1;a=create();CHECK(a>0);snapshot();CHECK(ipc_grant(&tasks[1],&tasks[0],a,RIGHT_SEND)==SF_NO_SPACE&&same());
 setup();for(unsigned owner=0;owner<2;owner++){current=owner;for(unsigned i=0;i<32;i++){long h=create();CHECK(h>0);if(!owner&&!i)a=h;}}
 CHECK(owners()==64);current=2;snapshot();CHECK(create()==SF_NO_SPACE&&same());
 receiver=ipc_grant(&tasks[0],&tasks[2],a,RIGHT_SEND);CHECK(receiver>0&&owners()==64);
 current=0;CHECK(!ipc_syscall(SYS_CAP_CLOSE,a,0,0)&&owners()==64);snapshot();CHECK(create()==SF_NO_SPACE&&same());
 current=2;CHECK(!ipc_syscall(SYS_CAP_CLOSE,receiver,0,0)&&owners()==63);current=0;CHECK(create()>0&&owners()==64);
 /* A thousand grow/fill/refuse/close traces retain generations and pool size. */
 setup();uint32_t seed=0x65ab13u;
 for(unsigned trial=0;trial<1000;trial++){
  seed=1664525u*seed+1013904223u;unsigned budget=seed%33;tasks[0].cap_limit=budget;
  for(unsigned i=0;i<budget;i++){handles[i]=create();CHECK(handles[i]>0&&task_caps_used(&tasks[0])==i+1&&owners()==i+1);}
  snapshot();for(unsigned i=0;i<3;i++)CHECK(create()==SF_NO_SPACE&&same());
  for(unsigned i=budget;i;i--){CHECK(!ipc_syscall(SYS_CAP_CLOSE,handles[i-1],0,0));CHECK(ipc_syscall(SYS_CAP_CLOSE,handles[i-1],0,0)==SF_STALE_HANDLE);}
  CHECK(!task_caps_used(&tasks[0])&&!owners());tasks[0].cap_limit=2;
  a=create();b=ipc_syscall(SYS_CAP_DERIVE,a,0,0);CHECK(a>0&&b>0&&owners()==1&&task_caps_used(&tasks[0])==2);
  snapshot();CHECK(create()==SF_NO_SPACE&&same());CHECK(!ipc_syscall(SYS_CAP_CLOSE,a,0,0)&&owners()==1);
  CHECK(!ipc_syscall(SYS_CAP_CLOSE,b,0,0)&&!owners()&&!task_caps_used(&tasks[0]));
 }
 printf("IPC BUDGET PASS: %u checks; 1000 quota traces, create/derive/grant admission, high slots remain usable, retired slots/global pool and exact failure-state preservation\n",checks);return 0;
}
