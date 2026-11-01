#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "../../kernel/ipc.c"
Task tasks[SF_MAX_TASKS];int current;uint64_t ticks;
static Frame frames[2];static SfMessage buffers[2];static Task saved_tasks[SF_MAX_TASKS];static Endpoint saved_eps[ENDPOINTS];
static unsigned checks;static int fail_in,fail_out,expected_panic;static jmp_buf recovery;
static unsigned __int128 actual_input,actual_output;
static uint32_t root_handle,peer_handle;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"IPC COPY FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int user_range(Task*t,uint64_t p,uint64_t n,int write){(void)write;if(t<tasks||t>=tasks+2)return 0;uint64_t base=(uintptr_t)&buffers[t-tasks];return p>=base&&p-base<=sizeof(SfMessage)&&n<=sizeof(SfMessage)-(p-base);}
int copy_in(Task*t,void*out,uint64_t p,size_t n){if(fail_in||!user_range(t,p,n,0))return SF_DENIED;memcpy(out,(void*)(uintptr_t)p,n);actual_input+=n;return 0;}
int copy_out(Task*t,uint64_t p,const void*in,size_t n){if(fail_out||!user_range(t,p,n,1))return SF_DENIED;memcpy((void*)(uintptr_t)p,in,n);actual_output+=n;return 0;}
void panic(const char*s){if(expected_panic){expected_panic=0;longjmp(recovery,1);}fprintf(stderr,"Unexpected panic: %s\n",s);abort();}
static void audit(void){
 struct {uint64_t before;SfIpcStats s;uint64_t after;} out;memset(&out,0xa5,sizeof(out));
 memcpy(saved_tasks,tasks,sizeof(tasks));memcpy(saved_eps,endpoints,sizeof(endpoints));ipc_stats_snapshot(&out.s);
 CHECK(out.before==0xa5a5a5a5a5a5a5a5ull&&out.after==out.before);
 CHECK(out.s.size==24&&out.s.version==1);
 CHECK(out.s.send_bytes==(actual_input>UINT64_MAX?UINT64_MAX:(uint64_t)actual_input));
 CHECK(out.s.receive_bytes==(actual_output>UINT64_MAX?UINT64_MAX:(uint64_t)actual_output));
 CHECK(!memcmp(saved_tasks,tasks,sizeof(tasks))&&!memcmp(saved_eps,endpoints,sizeof(endpoints)));
}
static void setup(uint64_t id){
 memset(tasks,0,sizeof(tasks));memset(endpoints,0,sizeof(endpoints));memset(frames,0,sizeof(frames));memset(buffers,0,sizeof(buffers));fail_in=fail_out=0;
 for(unsigned i=0;i<2;i++){tasks[i].pid=i+1;tasks[i].state=T_READY;tasks[i].cap_limit=32;tasks[i].frame=&frames[i];}
 current=0;ticks=100;root_handle=ipc_syscall(SYS_EP_CREATE,0,0,0);peer_handle=ipc_grant(&tasks[0],&tasks[1],root_handle,RIGHT_SEND|RIGHT_RECV);CHECK(root_handle&&peer_handle);
 buffers[0].h=(SfIpcHeader){40,1,0,OP_ECHO,99,id,sizeof(SfMessage)-40,0,0};buffers[0].length=id%257;
}
static long send(void){return ipc_syscall(SYS_SEND,root_handle,(uintptr_t)&buffers[0],0);}
static long receive(void){return ipc_syscall(SYS_RECV,root_handle,(uintptr_t)&buffers[0],0);}
int main(void){
 audit();
 for(unsigned trial=0;trial<1000;trial++){
  setup(trial+1);
  switch(trial%10){
   case 0:CHECK(!send()&&receive()==1);break;
   case 1:buffers[0].h.size=39;CHECK(send()==SF_INVALID);break;
   case 2:CHECK(ipc_syscall(SYS_SEND,(1ull<<32)|root_handle,(uintptr_t)&buffers[0],0)==SF_STALE_HANDLE);break;
   case 3:fail_in=1;CHECK(send()==SF_DENIED);fail_in=0;break;
   case 4:buffers[0].h.deadline_ns=1;CHECK(send()==SF_TIMEOUT);break;
   case 5:for(unsigned i=0;i<8;i++)CHECK(!send());CHECK(send()==SF_WOULD_BLOCK);for(unsigned i=0;i<8;i++)CHECK(receive()==1);break;
   case 6:{CHECK(!send());Endpoint queued=endpoints[0];SfMessage original=buffers[0];fail_out=1;
    CHECK(receive()==SF_DENIED&&!memcmp(&queued,&endpoints[0],sizeof(queued))&&!memcmp(&original,&buffers[0],sizeof(original)));fail_out=0;CHECK(receive()==1);break;}
   case 7:CHECK(!send());CHECK(ipc_syscall(SYS_RECV,root_handle,(uintptr_t)&buffers[0]+1,0)==SF_DENIED);CHECK(ipc_syscall(SYS_SEND_CANCEL,root_handle,trial+1,0)==1);break;
   case 8:current=1;CHECK(!ipc_syscall(SYS_RECV,peer_handle,(uintptr_t)&buffers[1],0)&&tasks[1].state==T_BLOCKED);current=0;
    CHECK(!send()&&tasks[1].state==T_READY&&frames[1].rax==1&&buffers[1].h.request_id==trial+1);break;
   case 9:for(unsigned i=0;i<8;i++)CHECK(!send());CHECK(!ipc_syscall(SYS_CAP_REVOKE,root_handle,0,0));CHECK(ipc_syscall(SYS_SEND_CANCEL,root_handle,trial+1,0)==SF_STALE_HANDLE);break;
  }
  audit();
 }
 setup(1);CHECK(ipc_syscall(SYS_RECV,root_handle,(uintptr_t)&buffers[0],1)==SF_TIMEOUT);audit();
 setup(2);current=1;CHECK(!ipc_syscall(SYS_RECV,peer_handle,(uintptr_t)&buffers[1],0));current=0;fail_out=1;expected_panic=1;
 if(!setjmp(recovery)){send();CHECK(0);}fail_out=0;CHECK(tasks[1].state==T_BLOCKED);audit();
 setup(3);send_bytes=UINT64_MAX-5;actual_input=UINT64_MAX-5;CHECK(!send());audit();
 receive_bytes=UINT64_MAX-1;actual_output=UINT64_MAX-1;CHECK(receive()==1);audit();CHECK(send_bytes==UINT64_MAX&&receive_bytes==UINT64_MAX);
 CHECK(!send()&&receive()==1);audit();CHECK(send_bytes==UINT64_MAX&&receive_bytes==UINT64_MAX);
 printf("IPC COPY PASS: %u checks; independent successful-copy event oracle, 1000 queue/direct/error/withdraw/revoke traces, failed receive preserves queue, snapshot guards, panic and 128-bit saturation oracle\n",checks);return 0;
}
