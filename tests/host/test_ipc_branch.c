/* Targeted host matrix for branch-only capability revocation. */
#define IPC_TEST_ENTRY ipc_baseline_main
#include "test_ipc.c"
#undef IPC_TEST_ENTRY

static long revoke_branch(long h){return call(SYS_CAP_REVOKE_BRANCH,(uint64_t)(unsigned)h,0,0);}

static void state_unchanged_reject(uint64_t h,uint64_t b,uint64_t c,long expected){
 static Task saved_tasks[SF_MAX_THREADS];static Frame saved_frames[SF_MAX_THREADS];
 static SfMessage saved_messages[SF_MAX_THREADS];unsigned before=copies;
 memcpy(saved_tasks,tasks,sizeof(tasks));memcpy(saved_frames,frames,sizeof(frames));
 memcpy(saved_messages,messages,sizeof(messages));
 CHECK(call(SYS_CAP_REVOKE_BRANCH,h,b,c)==expected);
 CHECK(copies==before&&!memcmp(tasks,saved_tasks,sizeof(tasks))&&
       !memcmp(frames,saved_frames,sizeof(frames))&&
       !memcmp(messages,saved_messages,sizeof(messages)));
}

static void owner_branch_is_narrow_and_reclaims_last_ref(void){
 reset();long root=create();CHECK(root>0);unsigned ep=endpoint(root);
 long branch=call(SYS_CAP_DERIVE,root,7,0);
 long child=call(SYS_CAP_DERIVE,branch,7,0);
 long sibling=call(SYS_CAP_DERIVE,root,7,0);CHECK(branch>0&&child>0&&sibling>0);
 CHECK(!revoke_branch(branch));CHECK(send(branch)==SF_STALE_HANDLE&&send(child)==SF_STALE_HANDLE);
 message();CHECK(!send(sibling));CHECK(receive(root,0)==1);
 message();CHECK(!send(root));CHECK(receive(root,0)==1);
 CHECK(!call(SYS_CAP_CLOSE,root,0,0));CHECK(!call(SYS_CAP_CLOSE,sibling,0,0));
 CHECK(send(root)==SF_STALE_HANDLE&&send(sibling)==SF_STALE_HANDLE);
 long replacement=create();CHECK(replacement>0&&endpoint(replacement)==ep);
 CHECK(send(root)==SF_STALE_HANDLE&&send(sibling)==SF_STALE_HANDLE);
}

static void cross_task_closed_chain_queue_ring(void){
 reset();long source=create();CHECK(source>0);unsigned source_ep=endpoint(source);
 long parent=ipc_grant(&tasks[0],&tasks[1],(uint32_t)source,7);CHECK(parent>0);
 current=2;long inbox=create();CHECK(inbox>0);
 long inbox_send=ipc_grant(&tasks[2],&tasks[1],(uint32_t)inbox,RIGHT_SEND);CHECK(inbox_send>0);
 current=3;long unrelated=create();CHECK(unrelated>0);
 current=1;long middle=call(SYS_CAP_DERIVE,parent,7,0);
 long child=call(SYS_CAP_DERIVE,middle,7,0);
 long sibling=ipc_grant(&tasks[0],&tasks[1],(uint32_t)source,7);
 CHECK(middle>0&&child>0&&sibling>0);
 /* Fill then drain part of the ring so subsequent attached capabilities wrap. */
 for(unsigned i=0;i<6;i++){message();messages[1].h.request_id=i+1;CHECK(!send(inbox_send));}
 current=2;for(unsigned i=0;i<5;i++)CHECK(receive(inbox,0)==2&&messages[2].status==0);
 current=1;
 for(unsigned i=0;i<7;i++){
  message();messages[1].h.request_id=100+i;messages[1].h.cap_count=1;
  messages[1].handle=(uint64_t)(unsigned)child;CHECK(!send(inbox_send));
 }
 unsigned middle_slot=((unsigned)middle&255)-1;
 CHECK(!call(SYS_CAP_CLOSE,middle,0,0));
 current=3;long unrelated_in_parent=ipc_grant(&tasks[3],&tasks[1],(uint32_t)unrelated,7);
 CHECK(unrelated_in_parent>0&&(((unsigned)unrelated_in_parent&255)-1)==middle_slot);
 current=2;CHECK(receive(inbox,0)==2&&messages[2].status==0&&messages[2].h.cap_count==0);
 CHECK(receive(inbox,0)==2&&messages[2].status==0&&messages[2].h.cap_count==1);
 long transferred=(long)messages[2].handle;CHECK(transferred>0);
 message();CHECK(!send(transferred));current=0;CHECK(receive(source,0)==3);

 current=1;CHECK(!revoke_branch(parent));
 CHECK(send(parent)==SF_STALE_HANDLE&&send(child)==SF_STALE_HANDLE);
 current=2;CHECK(send(transferred)==SF_STALE_HANDLE);
 for(unsigned i=0;i<6;i++)
  CHECK(receive(inbox,0)==2&&messages[2].status==SF_REVOKED&&
        messages[2].h.cap_count==0&&messages[2].handle==0);
 current=1;message();CHECK(!send(unrelated_in_parent));current=3;CHECK(receive(unrelated,0)==2);
 current=1;message();CHECK(!send(sibling));current=0;CHECK(receive(source,0)==2);
 CHECK(source_ep==endpoint(source));
 message();CHECK(!send(source));CHECK(receive(source,0)==1);
}

static void old_owner_revoke_stays_endpoint_wide(void){
 reset();long root=create();CHECK(root>0);
 long branch=call(SYS_CAP_DERIVE,root,7,0);
 long sibling=call(SYS_CAP_DERIVE,root,7,0);CHECK(branch>0&&sibling>0);
 CHECK(!call(SYS_CAP_REVOKE,root,0,0));
 CHECK(send(root)==SF_STALE_HANDLE&&send(branch)==SF_STALE_HANDLE&&send(sibling)==SF_STALE_HANDLE);
 long replacement=create();CHECK(replacement>0);CHECK(send(root)==SF_STALE_HANDLE);
}

static void branch_validation_is_atomic(void){
 reset();long root=create();CHECK(root>0);
 long branch=call(SYS_CAP_DERIVE,root,7,0);CHECK(branch>0);
 long limited=call(SYS_CAP_DERIVE,root,RIGHT_SEND,0);CHECK(limited>0);
 message();messages[0].h.request_id=0xbad;CHECK(!send(root));
 state_unchanged_reject(limited,0,0,SF_DENIED);
 state_unchanged_reject(0,0,0,SF_DENIED);
 state_unchanged_reject(UINT64_MAX,0,0,SF_DENIED);
 state_unchanged_reject(root|(1ull<<32),0,0,SF_DENIED);
 state_unchanged_reject(root,1,0,SF_INVALID);
 state_unchanged_reject(root,0,1,SF_INVALID);
 state_unchanged_reject(root,UINT64_MAX,0,SF_INVALID);
 state_unchanged_reject(root,0,UINT64_MAX,SF_INVALID);
 state_unchanged_reject(limited,1,1,SF_DENIED);
 CHECK(!call(SYS_CAP_CLOSE,branch,0,0));
 state_unchanged_reject(branch,0,0,SF_DENIED);
 CHECK(receive(root,0)==1&&messages[0].h.request_id==0xbad);
 message();CHECK(!send(root));CHECK(receive(root,0)==1);
 CHECK(!call(SYS_CAP_CLOSE,root,0,0));
 state_unchanged_reject(root,0,0,SF_DENIED);
}

static void zero_cap_limit_still_revokes(void){
 reset();long root=create();CHECK(root>0);
 long branch=call(SYS_CAP_DERIVE,root,7,0);
 long child=call(SYS_CAP_DERIVE,branch,7,0);CHECK(branch>0&&child>0);
 tasks[0].cap_limit=0;CHECK(!revoke_branch(branch));
 CHECK(send(branch)==SF_STALE_HANDLE&&send(child)==SF_STALE_HANDLE);
 CHECK(!call(SYS_CAP_CLOSE,root,0,0));
}

static void generation_and_endpoint_reuse_1000(void){
 reset();long previous=0;
 for(unsigned i=0;i<1000;i++){
  long root=create();CHECK(root>0);unsigned ep=endpoint(root);
  long branch=call(SYS_CAP_DERIVE,root,7,0);
  long child=call(SYS_CAP_DERIVE,branch,7,0);CHECK(branch>0&&child>0);
  CHECK(!revoke_branch(branch));CHECK(send(branch)==SF_STALE_HANDLE&&send(child)==SF_STALE_HANDLE);
  CHECK(!call(SYS_CAP_CLOSE,root,0,0));
  if(previous)CHECK(root!=previous&&send(previous)==SF_STALE_HANDLE);
  long replacement=create();CHECK(replacement>0&&endpoint(replacement)==ep);
  CHECK(send(root)==SF_STALE_HANDLE);previous=root;
  CHECK(!call(SYS_CAP_CLOSE,replacement,0,0));
 }
}

int main(void){
 owner_branch_is_narrow_and_reclaims_last_ref();
 cross_task_closed_chain_queue_ring();
 old_owner_revoke_stays_endpoint_wide();
 branch_validation_is_atomic();
 zero_cap_limit_still_revokes();
 generation_and_endpoint_reuse_1000();
 reset();printf("IPC branch revocation tests passed: %u assertions\n",checks);return 0;
}
