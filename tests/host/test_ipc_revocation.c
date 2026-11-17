/* Reuse the real IPC host fixture and exercise only revocation regressions. */
#define IPC_TEST_ENTRY ipc_baseline_main
#include "test_ipc.c"
#undef IPC_TEST_ENTRY

static void queued_cap_revoke_keeps_siblings(void){
 reset();long source=create();CHECK(source>0);
 long handed=ipc_grant(&tasks[0],&tasks[1],(uint32_t)source,7);
 current=2;long inbox=create();CHECK(handed>0&&inbox>0);
 long inbox_send=ipc_grant(&tasks[2],&tasks[1],(uint32_t)inbox,RIGHT_SEND);CHECK(inbox_send>0);
 current=1;long branch=call(SYS_CAP_DERIVE,handed,7,0);
 long sibling=call(SYS_CAP_DERIVE,handed,7,0);
 long attached=call(SYS_CAP_DERIVE,branch,7,0);CHECK(branch>0&&sibling>0&&attached>0);
 message();messages[1].h.cap_count=1;messages[1].handle=(uint64_t)(unsigned)attached;
 CHECK(!send(inbox_send));CHECK(!call(SYS_CAP_REVOKE,branch,0,0));
 current=2;CHECK(receive(inbox,0)==2&&messages[2].status==SF_REVOKED&&
                   messages[2].h.cap_count==0&&messages[2].handle==0);
 current=1;CHECK(send(branch)==SF_STALE_HANDLE&&send(attached)==SF_STALE_HANDLE);
 message();CHECK(!send(sibling));current=0;CHECK(receive(source,0)==2);
 message();CHECK(!send(source));CHECK(receive(source,0)==1);
}

static void closed_parent_ancestor_revoke_and_reuse(void){
 reset();long source=create();CHECK(source>0);
 long handed=ipc_grant(&tasks[0],&tasks[1],(uint32_t)source,7);CHECK(handed>0);
 current=2;long inbox=create();CHECK(inbox>0);
 long inbox_send=ipc_grant(&tasks[2],&tasks[1],(uint32_t)inbox,RIGHT_SEND);CHECK(inbox_send>0);
 current=1;long middle=call(SYS_CAP_DERIVE,handed,7,0);
 long child=call(SYS_CAP_DERIVE,middle,7,0);CHECK(middle>0&&child>0);
 message();messages[1].h.cap_count=1;messages[1].handle=(uint64_t)(unsigned)child;
 CHECK(!send(inbox_send));message();messages[1].h.cap_count=1;
 messages[1].handle=(uint64_t)(unsigned)child;CHECK(!send(inbox_send));
 unsigned old_slot=((unsigned)middle&255)-1;
 CHECK(!call(SYS_CAP_CLOSE,middle,0,0));
 message();CHECK(!send(child));current=0;CHECK(receive(source,0)==2);
 current=2;CHECK(receive(inbox,0)==2&&messages[2].status==0&&messages[2].h.cap_count==1);
 long transferred=(long)messages[2].handle;
 unsigned source_ep=tasks[0].caps[((unsigned)source&255)-1].endpoint;
 CHECK(transferred>0&&tasks[2].caps[(transferred&255)-1].endpoint==source_ep);
 message();CHECK(!send(transferred));current=0;CHECK(receive(source,0)==3);
 current=2;long unrelated=create();CHECK(unrelated>0);
 long unrelated_in_main=ipc_grant(&tasks[2],&tasks[1],(uint32_t)unrelated,7);
 CHECK(unrelated_in_main>0&&(((unsigned)unrelated_in_main&255)-1)==old_slot&&
       ((unsigned)unrelated_in_main>>8)!=((unsigned)middle>>8));
 current=1;CHECK(!call(SYS_CAP_REVOKE,handed,0,0));
 CHECK(send(child)==SF_STALE_HANDLE);current=2;CHECK(send(transferred)==SF_STALE_HANDLE);
 current=1;message();CHECK(!send(unrelated_in_main));current=2;CHECK(receive(unrelated,0)==2);
 CHECK(receive(inbox,0)==2&&messages[2].status==SF_REVOKED&&
       messages[2].h.cap_count==0&&messages[2].handle==0);
 current=0;message();CHECK(!send(source));CHECK(receive(source,0)==1);
}

static void thread_derived_granted_and_queued_caps(void){
 reset();long source=create();CHECK(source>0);
 long handed=ipc_grant(&tasks[0],&tasks[1],(uint32_t)source,7);
 long sibling=ipc_grant(&tasks[0],&tasks[1],(uint32_t)source,7);
 current=2;long inbox=create();CHECK(handed>0&&sibling>0&&inbox>0);
 tasks[3].main=0;tasks[3].main_slot=1;tasks[3].pid=tasks[1].pid;tasks[3].tid=2;
 long inbox_send=ipc_grant(&tasks[2],&tasks[3],(uint32_t)inbox,RIGHT_SEND);CHECK(inbox_send>0);
 current=3;long derived=call(SYS_CAP_DERIVE,handed,7,0);
 long granted=ipc_grant(&tasks[3],&tasks[4],(uint32_t)handed,7);
 message();messages[3].h.cap_count=1;messages[3].handle=(uint64_t)(unsigned)handed;
 CHECK(derived>0&&granted>0&&!send(inbox_send));
 current=1;CHECK(!call(SYS_CAP_REVOKE,handed,0,0));
 CHECK(send(derived)==SF_STALE_HANDLE);
 current=4;CHECK(send(granted)==SF_STALE_HANDLE);
 current=2;CHECK(receive(inbox,0)==2&&messages[2].status==SF_REVOKED&&
                   messages[2].h.cap_count==0&&messages[2].handle==0);
 current=1;message();CHECK(!send(sibling));current=0;CHECK(receive(source,0)==2);
 message();CHECK(!send(source));CHECK(receive(source,0)==1);
}

/* A blocked thread shares its process capability table. Revoking its final
 * receive branch wakes it, while other processes keep the endpoint alive. */
static void shared_process_thread_waiter_wakes(void){
 reset();long owner=create();CHECK(owner>0);
 long receiver=ipc_grant(&tasks[0],&tasks[1],(uint32_t)owner,RIGHT_RECV|RIGHT_DELEGATE);
 long other=ipc_grant(&tasks[0],&tasks[2],(uint32_t)owner,RIGHT_RECV);
 CHECK(receiver>0&&other>0);
 tasks[3].main=0;tasks[3].main_slot=1;tasks[3].pid=tasks[1].pid;tasks[3].tid=2;
 current=3;CHECK(receive(receiver,0)==0&&tasks[3].state==T_BLOCKED&&
                   tasks[3].wait_endpoint==endpoint(receiver));
 current=1;CHECK(!call(SYS_CAP_REVOKE,receiver,0,0));
 CHECK(tasks[3].state==T_READY&&(int64_t)frames[3].rax==SF_REVOKED);
 current=0;message();CHECK(!send(owner));
 current=2;CHECK(receive(other,0)==1&&messages[2].h.request_id==123);
}

/* Two closed ancestry levels are revisited repeatedly to cover generation
 * changes without allowing an owner-wide endpoint revoke to mask the walk. */
static void closed_ancestor_chain_repeated(void){
 for(unsigned round=0;round<1000;round++){
  reset();long source=create();CHECK(source>0);
  long branch=ipc_grant(&tasks[0],&tasks[1],(uint32_t)source,7);CHECK(branch>0);
  current=1;long first=call(SYS_CAP_DERIVE,branch,7,0);
  long second=call(SYS_CAP_DERIVE,first,7,0);
  long leaf=call(SYS_CAP_DERIVE,second,7,0);CHECK(first>0&&second>0&&leaf>0);
  CHECK(!call(SYS_CAP_CLOSE,first,0,0));CHECK(!call(SYS_CAP_CLOSE,second,0,0));
  CHECK(!call(SYS_CAP_REVOKE,branch,0,0));CHECK(send(leaf)==SF_STALE_HANDLE);
  current=0;message();CHECK(!send(source));CHECK(receive(source,0)==1);
 }
}

static void queued_ring_wrap_repeated(void){
 for(unsigned round=0;round<3;round++){
  reset();long source=create();CHECK(source>0);current=2;long inbox=create();CHECK(inbox>0);
  long handed=ipc_grant(&tasks[0],&tasks[1],(uint32_t)source,7);
  long inbox_send=ipc_grant(&tasks[2],&tasks[1],(uint32_t)inbox,RIGHT_SEND);
  CHECK(handed>0&&inbox_send>0);
  current=1;long branch=call(SYS_CAP_DERIVE,handed,7,0);
  long attached=call(SYS_CAP_DERIVE,branch,7,0);CHECK(branch>0&&attached>0);
  for(unsigned i=0;i<6;i++){message();messages[1].h.request_id=i+1;CHECK(!send(inbox_send));}
  current=2;for(unsigned i=0;i<5;i++)CHECK(receive(inbox,0)==2&&messages[2].status==0);
  current=1;
  for(unsigned i=0;i<7;i++){
   message();messages[1].h.request_id=100+i;messages[1].h.cap_count=1;
   messages[1].handle=(uint64_t)(unsigned)attached;CHECK(!send(inbox_send));
  }
  CHECK(!call(SYS_CAP_REVOKE,branch,0,0));current=2;
  CHECK(receive(inbox,0)==2&&messages[2].status==0&&messages[2].h.cap_count==0);
  for(unsigned i=0;i<7;i++)
   CHECK(receive(inbox,0)==2&&messages[2].status==SF_REVOKED&&
         messages[2].h.cap_count==0&&messages[2].handle==0);
  ticks=1;CHECK(receive(inbox,10000000)==SF_TIMEOUT);
 }
}

int main(void){
 queued_cap_revoke_keeps_siblings();
 closed_parent_ancestor_revoke_and_reuse();
 shared_process_thread_waiter_wakes();
 closed_ancestor_chain_repeated();
 thread_derived_granted_and_queued_caps();
 queued_ring_wrap_repeated();
 reset();printf("IPC revocation tests passed: %u assertions\n",checks);return 0;
}
