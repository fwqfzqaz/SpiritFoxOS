#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/kernel.h"

Task tasks[SF_MAX_TASKS];
int current;
uint64_t ticks;
static Frame frames[SF_MAX_TASKS];
static SfMessage messages[SF_MAX_TASKS];
static unsigned checks,copies;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"IPC FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)

/* Only each task's message buffer is accessible to the copy stubs. The real
   endpoint implementation is linked separately, without replacing its logic. */
int user_range(Task*t,uint64_t address,uint64_t bytes,int write){
 (void)write;uint64_t base=(uint64_t)&messages[t-tasks];
 return address>=base&&address-base<=sizeof(SfMessage)&&bytes<=sizeof(SfMessage)-(address-base);
}
int copy_in(Task*t,void*out,uint64_t address,size_t bytes){copies++;if(!user_range(t,address,bytes,0))return SF_DENIED;memcpy(out,(void*)address,bytes);return 0;}
int copy_out(Task*t,uint64_t address,const void*in,size_t bytes){copies++;if(!user_range(t,address,bytes,1))return SF_DENIED;memcpy((void*)address,in,bytes);return 0;}
void panic(const char*message){fprintf(stderr,"unexpected IPC panic: %s\n",message);exit(1);}

static long call(unsigned operation,uint64_t a,uint64_t b,uint64_t c){return ipc_syscall(operation,a,b,c);}
static long create(void){return call(SYS_EP_CREATE,0,0,0);}
static unsigned endpoint(long handle){return tasks[current].caps[(handle&255)-1].endpoint;}
static void message(void){memset(&messages[current],0,sizeof(SfMessage));messages[current].h.size=40;messages[current].h.protocol_major=1;messages[current].h.payload_bytes=sizeof(SfMessage)-40;messages[current].h.request_id=123;}
static long send(long handle){return call(SYS_SEND,handle,(uint64_t)&messages[current],0);}
static long receive(long handle,uint64_t deadline){return call(SYS_RECV,handle,(uint64_t)&messages[current],deadline);}
static void reset(void){
 for(unsigned i=0;i<SF_MAX_TASKS;i++)ipc_cleanup(&tasks[i]);
 memset(tasks,0,sizeof(tasks));memset(frames,0,sizeof(frames));memset(messages,0,sizeof(messages));
 for(unsigned i=0;i<SF_MAX_TASKS;i++){tasks[i].pid=i+1;tasks[i].state=T_READY;tasks[i].frame=&frames[i];}
 ticks=0;current=0;copies=0;
}

static void churn_and_capacity(void){
 reset();long stale=0;
 for(unsigned i=0;i<256;i++){long h=create();CHECK(h>0);if(stale)CHECK(call(SYS_CAP_CLOSE,stale,0,0)==SF_STALE_HANDLE);CHECK(!call(SYS_CAP_CLOSE,h,0,0));stale=h;}
 long held[2][32];
 for(unsigned t=0;t<2;t++){current=t;for(unsigned i=0;i<32;i++){held[t][i]=create();CHECK(held[t][i]>0);}}
 current=2;CHECK(create()==SF_NO_SPACE);current=0;CHECK(!call(SYS_CAP_CLOSE,held[0][0],0,0));
 current=2;CHECK(create()>0);current=0;CHECK(call(SYS_CAP_CLOSE,held[0][0],0,0)==SF_STALE_HANDLE);
}

static void derived_references(void){
 reset();long owner=create();CHECK(owner>0);unsigned id=endpoint(owner);
 long derived=call(SYS_CAP_DERIVE,owner,RIGHT_SEND|RIGHT_RECV,0);CHECK(derived>0);CHECK(!call(SYS_CAP_CLOSE,owner,0,0));
 message();CHECK(!send(derived));CHECK(receive(derived,0)==1);CHECK(messages[0].h.request_id==123);
 current=1;long unrelated=create();CHECK(unrelated>0&&endpoint(unrelated)!=id);CHECK(!call(SYS_CAP_CLOSE,unrelated,0,0));
 current=0;CHECK(!call(SYS_CAP_CLOSE,derived,0,0));long replacement=create();CHECK(replacement>0&&endpoint(replacement)==id);
 CHECK(send(owner)==SF_STALE_HANDLE);CHECK(send(derived)==SF_STALE_HANDLE);
 CHECK(!call(SYS_CAP_CLOSE,replacement,0,0));
}

static void cross_task_references(void){
 reset();long owner=create();CHECK(owner>0);unsigned id=endpoint(owner);
 long transferred=ipc_grant(&tasks[0],&tasks[1],owner,RIGHT_SEND|RIGHT_RECV);CHECK(transferred>0);CHECK(!call(SYS_CAP_CLOSE,owner,0,0));
 current=2;long unrelated=create();CHECK(unrelated>0&&endpoint(unrelated)!=id);CHECK(!call(SYS_CAP_CLOSE,unrelated,0,0));
 current=1;message();CHECK(!send(transferred));CHECK(receive(transferred,0)==2);CHECK(!call(SYS_CAP_CLOSE,transferred,0,0));
 current=0;long replacement=create();CHECK(replacement>0&&endpoint(replacement)==id);
 current=1;CHECK(send(transferred)==SF_STALE_HANDLE);

 /* Process cleanup must also collect the last reference to an endpoint
    created by a different, still-live process. */
 reset();owner=create();CHECK(owner>0);id=endpoint(owner);transferred=ipc_grant(&tasks[0],&tasks[1],owner,RIGHT_RECV);CHECK(transferred>0);CHECK(!call(SYS_CAP_CLOSE,owner,0,0));
 ipc_cleanup(&tasks[1]);current=2;replacement=create();CHECK(replacement>0&&endpoint(replacement)==id);current=1;CHECK(receive(transferred,0)==SF_STALE_HANDLE);
}

static void revoke_and_owner_exit(void){
 reset();long owner=create();CHECK(owner>0);long receiver=ipc_grant(&tasks[0],&tasks[1],owner,RIGHT_RECV);CHECK(receiver>0);
 current=1;CHECK(receive(receiver,0)==0&&tasks[1].state==T_BLOCKED);
 current=0;CHECK(!call(SYS_CAP_REVOKE,owner,0,0));CHECK(tasks[1].state==T_READY&&(int64_t)frames[1].rax==SF_REVOKED);
 current=1;CHECK(receive(receiver,0)==SF_STALE_HANDLE);current=0;long replacement=create();CHECK(replacement>0);CHECK(call(SYS_CAP_REVOKE,owner,0,0)==SF_DENIED);
 receiver=ipc_grant(&tasks[0],&tasks[1],replacement,RIGHT_RECV);CHECK(receiver>0);current=1;CHECK(receive(receiver,0)==0&&tasks[1].state==T_BLOCKED);
 ipc_cleanup(&tasks[0]);CHECK(tasks[1].state==T_READY&&(int64_t)frames[1].rax==SF_REVOKED);CHECK(receive(receiver,0)==SF_STALE_HANDLE);
}

static void deadlines_and_validation(void){
 reset();long owner=create();CHECK(owner>0);message();CHECK(call(SYS_SEND,owner,0,0)==SF_DENIED);CHECK(call(SYS_RECV,owner,0,0)==SF_DENIED);
 messages[0].h.cap_count=1;CHECK(send(owner)==SF_INVALID);messages[0].h.cap_count=0;messages[0].length=SF_MSG_BYTES+1;CHECK(send(owner)==SF_INVALID);messages[0].length=0;
 for(unsigned i=0;i<8;i++)CHECK(!send(owner));CHECK(send(owner)==SF_WOULD_BLOCK);
 for(unsigned i=0;i<8;i++)CHECK(receive(owner,0)==1);
 CHECK(receive(owner,20000000)==0&&tasks[0].state==T_BLOCKED);ticks=1;ipc_deadlines();CHECK(tasks[0].state==T_BLOCKED);ticks=2;ipc_deadlines();CHECK(tasks[0].state==T_READY&&(int64_t)frames[0].rax==SF_TIMEOUT);
 messages[0].h.deadline_ns=20000000;CHECK(send(owner)==SF_TIMEOUT);
 long reduced=call(SYS_CAP_DERIVE,owner,RIGHT_SEND,0);CHECK(reduced>0);CHECK(call(SYS_CAP_DERIVE,reduced,RIGHT_RECV,0)==SF_DENIED);CHECK(receive(reduced,0)==SF_STALE_HANDLE);
}

static void retired_slot(void){
 reset();tasks[0].caps[0].generation=0x7fffff;long h=create();CHECK(h>0&&(h&255)==2);CHECK(!call(SYS_CAP_CLOSE,h,0,0));
 for(unsigned i=0;i<32;i++)tasks[0].caps[i].generation=0x7fffff;
 CHECK(create()==SF_NO_SPACE);current=1;CHECK(create()>0);
}

static void canonical_width(void){
 for(unsigned iteration=0;iteration<1000;iteration++){
  reset();long owner=create();CHECK(owner>0);current=1;long unrelated=create();CHECK(unrelated>0);current=0;message();
  Task saved_tasks[SF_MAX_TASKS];Frame saved_frames[SF_MAX_TASKS];SfMessage saved_messages[SF_MAX_TASKS];
  memcpy(saved_tasks,tasks,sizeof(tasks));memcpy(saved_frames,frames,sizeof(frames));memcpy(saved_messages,messages,sizeof(messages));
  uint64_t high[]={1ull<<32,1ull<<63,0xffffffff00000000ull};
  for(unsigned i=0;i<sizeof(high)/sizeof(*high);i++){
   uint64_t invalid=high[i]|(unsigned)owner;unsigned before=copies;
   CHECK(call(SYS_SEND,invalid,0,0)==SF_STALE_HANDLE);CHECK(call(SYS_RECV,invalid,0,0)==SF_STALE_HANDLE);
   CHECK(call(SYS_CAP_CLOSE,invalid,0,0)==SF_STALE_HANDLE);CHECK(call(SYS_CAP_DERIVE,invalid,7,0)==SF_DENIED);CHECK(call(SYS_CAP_REVOKE,invalid,0,0)==SF_DENIED);
   CHECK(copies==before);CHECK(!memcmp(tasks,saved_tasks,sizeof(tasks)));CHECK(!memcmp(frames,saved_frames,sizeof(frames)));CHECK(!memcmp(messages,saved_messages,sizeof(messages)));
  }
  CHECK(!send(owner));CHECK(receive(owner,0)==1);CHECK(messages[0].h.request_id==123&&messages[0].h.flags==1);
  long duplicate=call(SYS_CAP_DERIVE,owner,RIGHT_SEND|RIGHT_RECV,0);CHECK(duplicate>0);CHECK(!call(SYS_CAP_CLOSE,owner,0,0));message();CHECK(!send(duplicate));CHECK(receive(duplicate,0)==1);CHECK(!call(SYS_CAP_CLOSE,duplicate,0,0));
  current=1;message();CHECK(!send(unrelated));CHECK(receive(unrelated,0)==2);CHECK(!call(SYS_CAP_CLOSE,unrelated,0,0));
 }
}

int main(void){canonical_width();churn_and_capacity();derived_references();cross_task_references();revoke_and_owner_exit();deadlines_and_validation();retired_slot();reset();printf("IPC tests passed: %u assertions\n",checks);return 0;}
