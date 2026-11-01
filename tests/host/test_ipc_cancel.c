#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/ipc.c"
Task tasks[SF_MAX_TASKS];int current;uint64_t ticks;
static Frame frames[SF_MAX_TASKS];static SfMessage buffers[SF_MAX_TASKS],saved_buffers[SF_MAX_TASKS];
static Task saved_tasks[SF_MAX_TASKS];static Endpoint saved_eps[ENDPOINTS];
static unsigned checks,copies;static uint32_t handles[2];static unsigned ep;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"IPC CANCEL FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int user_range(Task*t,uint64_t p,uint64_t n,int write){(void)write;uint64_t base=(uintptr_t)&buffers[t-tasks];return p>=base&&p-base<=sizeof(SfMessage)&&n<=sizeof(SfMessage)-(p-base);}
int copy_in(Task*t,void*out,uint64_t p,size_t n){copies++;if(!user_range(t,p,n,0))return SF_DENIED;memcpy(out,(void*)(uintptr_t)p,n);return 0;}
int copy_out(Task*t,uint64_t p,const void*in,size_t n){copies++;if(!user_range(t,p,n,1))return SF_DENIED;memcpy((void*)(uintptr_t)p,in,n);return 0;}
void panic(const char*s){fprintf(stderr,"Unexpected panic: %s\n",s);abort();}
static void setup(void){
 memset(tasks,0,sizeof(tasks));memset(endpoints,0,sizeof(endpoints));memset(frames,0,sizeof(frames));memset(buffers,0,sizeof(buffers));
 for(unsigned i=0;i<SF_MAX_TASKS;i++){tasks[i].pid=i+1;tasks[i].state=T_READY;tasks[i].cap_limit=32;tasks[i].frame=&frames[i];tasks[i].priority=1;tasks[i].ready_since=3;}
 current=0;ticks=100;handles[0]=ipc_syscall(SYS_EP_CREATE,0,0,0);CHECK((int)handles[0]>0);
 handles[1]=ipc_grant(&tasks[0],&tasks[1],handles[0],RIGHT_SEND|RIGHT_RECV);CHECK((int)handles[1]>0);
 ep=tasks[0].caps[(handles[0]&255)-1].endpoint-1;
}
static SfMessage message(unsigned actor,uint64_t id,unsigned tag){
 SfMessage m={0};m.h.size=40;m.h.protocol_major=1;m.h.opcode=OP_ECHO;m.h.flags=99;m.h.request_id=id;m.h.payload_bytes=sizeof(m)-40;
 m.status=tag;m.offset=id^0xa5;m.handle=0x1234;m.length=256;
 for(unsigned i=0;i<sizeof(m.data);i++)m.data[i]=(uint8_t)(tag+i);
 (void)actor;return m;
}
static SfMessage send_to(unsigned actor,uint32_t handle,uint64_t id,unsigned tag){
 current=(int)actor;buffers[actor]=message(actor,id,tag);CHECK(!ipc_syscall(SYS_SEND,handle,(uintptr_t)&buffers[actor],0));
 SfMessage expected=buffers[actor];expected.h.flags=actor+1;return expected;
}
static SfMessage receive(unsigned actor,uint32_t handle){
 current=(int)actor;long r=ipc_syscall(SYS_RECV,handle,(uintptr_t)&buffers[actor],ticks*10000000ull);
 CHECK(r>0&&(uint32_t)r==buffers[actor].h.flags);return buffers[actor];
}
static void snapshot(void){memcpy(saved_tasks,tasks,sizeof(tasks));memcpy(saved_eps,endpoints,sizeof(endpoints));memcpy(saved_buffers,buffers,sizeof(buffers));}
static long cancel(unsigned actor,uint64_t handle,uint64_t id,uint64_t reserved){
 current=(int)actor;unsigned before=copies;long r=ipc_syscall(SYS_SEND_CANCEL,handle,id,reserved);
 CHECK(copies==before&&!memcmp(saved_tasks,tasks,sizeof(tasks))&&!memcmp(saved_buffers,buffers,sizeof(buffers)));return r;
}
static void prime(unsigned head){for(unsigned i=0;i<head;i++){send_to(0,handles[0],0x9911+i,0);receive(0,handles[0]);}CHECK(endpoints[ep].head==head);}
typedef struct {unsigned actor;uint64_t id;} Entry;
typedef struct {unsigned head,actor;uint64_t id;unsigned count,removed;Entry entries[8];unsigned keep[8];} Vector;
int main(void){
 const uint64_t high=0x100000007ull;
 const Vector vectors[]={
  {3,0,7,0,0,{{0,0}},{0}},
  {0,0,7,3,1,{{0,7},{1,8},{0,9}},{1,2}},
  /* Independent helper's A:8 vs B:8 false positive is corrected here. */
  {2,0,8,3,0,{{0,7},{1,8},{0,9}},{0,1,2}},
  {2,1,8,3,1,{{0,7},{1,8},{0,9}},{0,2}},
  {5,0,9,3,1,{{0,7},{1,8},{0,9}},{0,1}},
  {0,0,7,4,2,{{0,7},{1,7},{0,7},{1,8}},{1,3}},
  {5,1,7,3,1,{{0,7},{1,7},{0,7}},{0,2}},
  {1,0,high,2,1,{{0,7},{0,high}},{0}},
  {2,0,0,3,1,{{0,0},{1,0},{0,5}},{1,2}},
  {6,0,high,8,1,{{0,1},{1,2},{0,3},{1,4},{0,5},{1,6},{0,high},{1,8}},{0,1,2,3,4,5,7}},
  {4,0,7,2,0,{{0,8},{1,9}},{0,1}},
  {7,0,0,8,8,{{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0}},{0}}
 };
 for(unsigned v=0;v<sizeof(vectors)/sizeof(*vectors);v++){
  const Vector*x=&vectors[v];setup();prime(x->head);SfMessage expected[8];
  for(unsigned i=0;i<x->count;i++)expected[i]=send_to(x->entries[i].actor,handles[x->entries[i].actor],x->entries[i].id,i+1);
  snapshot();CHECK(cancel(x->actor,handles[x->actor],x->id,0)==(long)x->removed);
  CHECK(endpoints[ep].owner==saved_eps[ep].owner&&endpoints[ep].epoch==saved_eps[ep].epoch&&endpoints[ep].count==x->count-x->removed);
  if(!x->removed)CHECK(!memcmp(saved_eps,endpoints,sizeof(endpoints)));
  for(unsigned i=0;i<x->count-x->removed;i++){SfMessage actual=receive(0,handles[0]);CHECK(!memcmp(&actual,&expected[x->keep[i]],sizeof(actual)));}
  current=0;CHECK(ipc_syscall(SYS_RECV,handles[0],(uintptr_t)&buffers[0],ticks*10000000ull)==SF_TIMEOUT);
  snapshot();CHECK(cancel(x->actor,handles[x->actor],x->id,0)==0&&!memcmp(saved_eps,endpoints,sizeof(endpoints)));
 }
 /* Full-width handles, rights, reserved fields and private count bounds. */
 setup();send_to(0,handles[0],7,1);const uint64_t invalid[]={0,1ull<<32|handles[0],1ull<<63|handles[0],UINT64_MAX};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){snapshot();CHECK(cancel(0,invalid[i],7,0)==SF_STALE_HANDLE&&!memcmp(saved_eps,endpoints,sizeof(endpoints)));}
 snapshot();CHECK(cancel(0,handles[0],7,1)==SF_INVALID&&!memcmp(saved_eps,endpoints,sizeof(endpoints)));
 snapshot();CHECK(cancel(0,handles[0],7,1ull<<63)==SF_INVALID&&!memcmp(saved_eps,endpoints,sizeof(endpoints)));
 int recv_only=ipc_grant(&tasks[0],&tasks[1],handles[0],RIGHT_RECV);CHECK(recv_only>0);snapshot();CHECK(cancel(1,recv_only,7,0)==SF_STALE_HANDLE&&!memcmp(saved_eps,endpoints,sizeof(endpoints)));
 endpoints[ep].count=9;snapshot();CHECK(cancel(0,handles[0],7,0)==SF_BAD_FORMAT&&!memcmp(saved_eps,endpoints,sizeof(endpoints)));endpoints[ep].count=1;
 endpoints[ep].head=8;snapshot();CHECK(cancel(0,handles[0],7,0)==SF_BAD_FORMAT&&!memcmp(saved_eps,endpoints,sizeof(endpoints)));
 /* Identical request IDs on another endpoint remain independent. */
 setup();current=0;uint32_t other=ipc_syscall(SYS_EP_CREATE,0,0,0);CHECK((int)other>0);unsigned other_ep=tasks[0].caps[(other&255)-1].endpoint-1;
 send_to(0,handles[0],7,1);SfMessage other_message=send_to(0,other,7,2);snapshot();
 CHECK(cancel(0,handles[0],7,0)==1&&!memcmp(&saved_eps[other_ep],&endpoints[other_ep],sizeof(Endpoint)));
 SfMessage other_received=receive(0,other);CHECK(!memcmp(&other_message,&other_received,sizeof(other_message)));
 /* Retained references do not revive a closed or revoked caller handle. */
 setup();send_to(0,handles[0],7,1);current=0;CHECK(!ipc_syscall(SYS_CAP_CLOSE,handles[0],0,0));snapshot();
 CHECK(cancel(0,handles[0],7,0)==SF_STALE_HANDLE&&!memcmp(saved_eps,endpoints,sizeof(endpoints)));
 setup();send_to(1,handles[1],7,1);current=0;CHECK(!ipc_syscall(SYS_CAP_REVOKE,handles[0],0,0));snapshot();
 CHECK(cancel(1,handles[1],7,0)==SF_STALE_HANDLE&&!memcmp(saved_eps,endpoints,sizeof(endpoints)));
 /* Direct delivery has crossed the boundary: withdrawal cannot undo output. */
 setup();current=1;CHECK(!ipc_syscall(SYS_RECV,handles[1],(uintptr_t)&buffers[1],0)&&tasks[1].state==T_BLOCKED);
 SfMessage delivered=send_to(0,handles[0],UINT64_MAX,77);CHECK(tasks[1].state==T_READY&&!memcmp(&delivered,&buffers[1],sizeof(delivered)));
 snapshot();CHECK(cancel(0,handles[0],UINT64_MAX,0)==0&&!memcmp(saved_eps,endpoints,sizeof(endpoints)));
 /* Withdrawal does not need a spare capability slot. */
 setup();for(unsigned i=1;i<32;i++)CHECK(ipc_syscall(SYS_CAP_DERIVE,handles[0],RIGHT_SEND,0)>0);
 send_to(0,handles[0],0,9);snapshot();CHECK(cancel(0,handles[0],0,0)==1&&task_caps_used(&tasks[0])==32&&endpoints[ep].owner==1);
 /* One thousand independent logical filters exercise every physical head. */
 uint32_t seed=0x815ca7u;
 for(unsigned trial=0;trial<1000;trial++){
  setup();prime(trial%8);seed=1664525u*seed+1013904223u;unsigned count=seed%9,actor=(seed>>5)&1;uint64_t target=trial%7==0?UINT64_MAX:(seed>>9)%5;
  SfMessage expected[8];unsigned keep[8],kept=0;
  for(unsigned i=0;i<count;i++){
   seed=1664525u*seed+1013904223u;unsigned sender=(seed>>4)&1;uint64_t id=seed%5;if(i==count/2&&trial%3==0)id=target;
   expected[i]=send_to(sender,handles[sender],id,trial*8+i+1);if(sender!=actor||id!=target)keep[kept++]=i;
  }
  snapshot();CHECK(cancel(actor,handles[actor],target,0)==(long)(count-kept));CHECK(endpoints[ep].count==kept&&endpoints[ep].epoch==saved_eps[ep].epoch);
  if(count==kept)CHECK(!memcmp(saved_eps,endpoints,sizeof(endpoints)));
  for(unsigned i=0;i<kept;i++){SfMessage actual=receive(0,handles[0]);CHECK(!memcmp(&actual,&expected[keep[i]],sizeof(actual)));}
  current=0;CHECK(ipc_syscall(SYS_RECV,handles[0],(uintptr_t)&buffers[0],ticks*10000000ull)==SF_TIMEOUT);
 }
 printf("IPC CANCEL PASS: %u checks; authenticated sender/full ID, FIFO wrap/filter, 1000 logical traces, no-match state, rights/bounds, direct delivery and full capability budget\n",checks);return 0;
}
