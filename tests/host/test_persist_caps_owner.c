/* E1: actual Root provisional-cap owner with synthetic guest ACKs. */
#define SF_VERIFY_OWNER_ENTRY verify_owner_baseline_main
#define SF_TEST_STAGE_OWNER
#include "test_persist_verify_owner.c"
#undef SF_TEST_STAGE_OWNER
#undef SF_VERIFY_OWNER_ENTRY

static int caps_hook_enabled;
static unsigned derive_calls,close_calls,branch_revoke_calls,stage_send_calls,stage_recv_calls,usage_queries,usage_history[4];
static unsigned candidate_usage,stage_deliveries[2],stage_fail_send,stage_fail_recv,stage_deliver_on_failed_send;
static unsigned stage_bad_ack,stage_bad_ack_at,stage_bad_from;
static int stage_send_error,stage_recv_error,stage_peer_status,cap_close_error,branch_revoke_error[2];
static int64_t derive_result[2];
static uint64_t derive_parent[2],closed_parent[4],revoked_parent[4];
static unsigned derive_rights[2];
static SfMessage stage_request;

#define CAPS_CHECK(c) CHECK(c)

static void reset_caps_hooks(void){
 caps_hook_enabled=0;derive_calls=close_calls=branch_revoke_calls=stage_send_calls=stage_recv_calls=usage_queries=0;
 memset(usage_history,0,sizeof(usage_history));
 candidate_usage=2;stage_deliveries[0]=stage_deliveries[1]=0;
 stage_fail_send=stage_fail_recv=stage_deliver_on_failed_send=0;
 stage_bad_ack=stage_bad_ack_at=stage_bad_from=0;
 stage_send_error=stage_recv_error=stage_peer_status=cap_close_error=0;
 branch_revoke_error[0]=branch_revoke_error[1]=0;
 derive_result[0]=0x601;derive_result[1]=0x602;
 memset(derive_parent,0,sizeof(derive_parent));memset(derive_rights,0,sizeof(derive_rights));
 memset(closed_parent,0,sizeof(closed_parent));memset(revoked_parent,0,sizeof(revoked_parent));
 memset(&stage_request,0,sizeof(stage_request));
}

static int caps_owner_sys_hook(long op,uint64_t a,uint64_t b,uint64_t c,int64_t*out){
 if(!caps_hook_enabled)return 0;
 if(op==SYS_CAP_DERIVE){
  unsigned i=derive_calls++;CAPS_CHECK(i<2);
  uint32_t expected=i?admission_fence.driver_reply:admission_fence.driver_ep;
  unsigned rights=i?(RIGHT_RECV|RIGHT_DELEGATE):(RIGHT_SEND|RIGHT_DELEGATE);
  CAPS_CHECK(a==expected&&b==rights&&!c);
  derive_parent[i]=a;derive_rights[i]=(unsigned)b;*out=derive_result[i];return 1;
 }
 if(op==SYS_CAP_CLOSE){
  unsigned i=close_calls++;CAPS_CHECK(i<4&&a!=admission_fence.driver_ep&&a!=admission_fence.driver_reply);
  closed_parent[i]=a;*out=cap_close_error;return 1;
 }
 if(op==SYS_CAP_REVOKE_BRANCH){
  unsigned i=branch_revoke_calls++;CAPS_CHECK(i<4&&a!=admission_fence.driver_ep&&a!=admission_fence.driver_reply);
  CAPS_CHECK((a==0x601||a==0x602)&&!b&&!c);revoked_parent[i]=a;
  int index=a==0x601?0:1;*out=branch_revoke_error[index];
  if(!*out&&stage_deliveries[index]){CAPS_CHECK(candidate_usage>2);candidate_usage--;stage_deliveries[index]=0;}
  return 1;
 }
 if(op==SYS_TASK_CAP_LIMIT&&a==13&&b==0&&c==SF_LIMIT_USED){
  unsigned i=usage_queries++;if(i<4)usage_history[i]=candidate_usage;*out=candidate_usage;return 1;
 }
 return 0;
}

static int caps_owner_send_cap_hook(uint32_t ep,SfMessage*m,uint64_t cap,int64_t*out){
 if(!caps_hook_enabled)return 0;
 CAPS_CHECK(ep==idle_candidate.ep&&m->h.opcode==OP_PERSIST_CANDIDATE_CAPS&&
            m->mode>=SF_PERSIST_STAGE_SEND&&m->mode<=SF_PERSIST_STAGE_REPLY);
 unsigned i=m->mode-1;stage_send_calls++;
 CAPS_CHECK(cap==candidate_caps.parent[i]&&m->h.cap_count==1&&m->handle==cap&&
            m->h.size==sizeof(SfIpcHeader)&&m->h.protocol_major==1&&!m->h.protocol_minor&&
            !m->h.flags&&m->h.request_id==candidate_caps.request_id[i]&&
            m->h.payload_bytes==sizeof(*m)-sizeof(SfIpcHeader)&&m->h.deadline_ns==candidate_caps.deadline[i]&&
            !m->status&&!m->offset&&m->length==sizeof(SfPersistStageBinding)&&
            m->mode==i+1&&!memcmp(m->data,&candidate_caps.binding,sizeof(candidate_caps.binding)));
 for(unsigned j=sizeof(SfPersistStageBinding);j<sizeof(m->data);j++)CAPS_CHECK(!m->data[j]);
 stage_request=*m;
 if(stage_fail_send==i+1){
  if(stage_deliver_on_failed_send){candidate_usage++;stage_deliveries[i]=1;}
  *out=stage_send_error;return 1;
 }
 candidate_usage++;stage_deliveries[i]=1;*out=0;return 1;
}

static void stage_ack_mutate(SfMessage*m,unsigned kind){
 switch(kind){
  case 1:m->h.size--;break;case 2:m->h.protocol_major++;break;case 3:m->h.protocol_minor++;
   break;case 4:m->h.opcode++;break;case 5:m->h.flags++;break;case 6:m->h.request_id++;
   break;case 7:m->h.cap_count=1;m->handle=0x777;break;case 8:m->h.cap_count=2;break;
  case 9:m->h.payload_bytes--;break;case 10:m->h.deadline_ns++;break;case 11:m->handle=1;break;
  case 12:m->offset=1;break;case 13:m->status=SF_BUSY;break;case 14:m->status=1ll<<32;break;
  case 15:m->mode=0;break;case 16:m->length--;break;case 17:m->data[0]^=1;break;
  case 18:m->data[sizeof(SfPersistStageBinding)]^=1;break;
  default:break;
 }
}

static int caps_owner_recv_hook(uint32_t ep,SfMessage*m,uint64_t deadline,int64_t*out){
 if(!caps_hook_enabled||m->h.opcode!=OP_PERSIST_CANDIDATE_CAPS)return 0;
 CAPS_CHECK(ep==idle_candidate.reply&&deadline==stage_request.h.deadline_ns);
 unsigned i=stage_recv_calls++;
 if(stage_fail_recv==i+1){*out=stage_recv_error;return 1;}
 *m=stage_request;m->h.flags=idle_candidate.pid;m->h.cap_count=0;m->status=stage_peer_status;
 m->mode=stage_request.mode;m->handle=m->offset=0;m->length=sizeof(SfPersistStageBinding);
 memset(m->data,0,sizeof(m->data));
 if(stage_peer_status){m->mode=0;m->length=0;}else memcpy(m->data,&candidate_caps.binding,sizeof(candidate_caps.binding));
 if(stage_bad_ack&&(!stage_bad_ack_at||stage_bad_ack_at==i+1))stage_ack_mutate(m,stage_bad_ack);
 *out=stage_bad_from?stage_bad_from:idle_candidate.pid;return 1;
}

static uint64_t prepare_hold_caps(void){
 caps_hook_enabled=0;uint64_t token=start_owner(259);memset(&candidate_caps,0,sizeof(candidate_caps));
 reset_caps_hooks();
 CHECK(!sf_persist_candidate_hold(token,verify_spawn,(void*)0x1234,verify_sector_read,0));
 SfPersistIdleStatus idle=verify_report(token);
 CHECK(idle.phase==SF_PERSIST_IDLE_PREPARED&&idle.verified&&idle.read_frames==259&&
       idle.guard_denied&&!idle.discard_accepted&&!idle.death_confirmed&&!idle.retired);
 stable_after_cycle(token);candidate_usage=2;caps_hook_enabled=1;return token;
}
static SfPersistCapsStatus caps_report(uint64_t token){
 SfPersistCapsStatus s;memset(&s,0xa5,sizeof(s));
 CHECK(!sf_persist_candidate_caps_status(token,&s)&&s.token==token);return s;
}
static void caps_pure_status(uint64_t token){
 SfPersistCapsStatus a=caps_report(token);unsigned d=derive_calls,cl=close_calls,rv=branch_revoke_calls,uq=usage_queries;
 unsigned se=stage_send_calls,re=stage_recv_calls,ep=verify_ep_calls,sp=verify_spawn_calls;
 SfPersistCapsStatus b=caps_report(token);CHECK(!memcmp(&a,&b,sizeof(a)));
 CHECK(d==derive_calls&&cl==close_calls&&rv==branch_revoke_calls&&se==stage_send_calls&&
       re==stage_recv_calls&&uq==usage_queries&&ep==verify_ep_calls&&sp==verify_spawn_calls);
 SfPersistCapsStatus untouched;memset(&untouched,0xa5,sizeof(untouched));
 CHECK(sf_persist_candidate_caps_status(token+1,&untouched)==SF_STALE_HANDLE);
 CHECK(d==derive_calls&&cl==close_calls&&rv==branch_revoke_calls&&se==stage_send_calls&&
       re==stage_recv_calls&&uq==usage_queries);
}
static void assert_no_stage_retry(uint64_t token,int result){
 unsigned d=derive_calls,cl=close_calls,rv=branch_revoke_calls,se=stage_send_calls,re=stage_recv_calls,uq=usage_queries;
 CHECK(sf_persist_candidate_stage_caps(token)==result);
 CHECK(d==derive_calls&&cl==close_calls&&rv==branch_revoke_calls&&se==stage_send_calls&&
       re==stage_recv_calls&&uq==usage_queries);
}

static void normal_stage_and_discard(void){
 uint64_t token=prepare_hold_caps();caps_pure_status(token);
 CHECK(!sf_persist_candidate_stage_caps(token));
 SfPersistCapsStatus s=caps_report(token);
 CHECK(s.phase==SF_PERSIST_CAPS_READY&&s.token==token&&s.parent[0]==0x601&&s.parent[1]==0x602&&
       !s.branches_retired&&!s.allocation_unknown&&!s.error&&s.derive_result[0]==0x601&&s.derive_result[1]==0x602);
 CHECK(derive_calls==2&&close_calls==0&&branch_revoke_calls==0&&stage_send_calls==2&&stage_recv_calls==2);
 CHECK(usage_queries==3&&usage_history[0]==2&&usage_history[1]==3&&usage_history[2]==4);
 CHECK(derive_parent[0]==0x201&&derive_parent[1]==0x202&&
       derive_rights[0]==(RIGHT_SEND|RIGHT_DELEGATE)&&derive_rights[1]==(RIGHT_RECV|RIGHT_DELEGATE));
 CHECK(candidate_usage==4&&candidate_caps.request_id[0]<candidate_caps.request_id[1]&&
       candidate_caps.deadline[0]==clock_ticks*10000000ull+SF_PERSIST_STAGE_NS&&
       candidate_caps.deadline[1]==clock_ticks*10000000ull+SF_PERSIST_STAGE_NS&&
       stage_request.h.opcode==OP_PERSIST_CANDIDATE_CAPS);
 CHECK(s.binding.snapshot.size==sizeof(SfPersistSnapshot)&&s.binding.token==token&&
       s.binding.storage_instance==admission_fence.storage_instance&&
       s.binding.driver_pid==22&&s.binding.fs_pid==21&&s.binding.capacity==capacity&&s.binding.writable==1);
 CHECK(idle_candidate.phase==SF_PERSIST_IDLE_STAGED&&idle_candidate.verified&&sf_persist_fenced());
 caps_pure_status(token);assert_no_stage_retry(token,SF_DRAINING);
 CHECK(sf_persist_fenced()&&sf_storage_transaction(1)==SF_DRAINING&&sf_persist_quiescent()==SF_DEVICE_GONE);

 uint64_t staged0=candidate_caps.request_id[0],staged1=candidate_caps.request_id[1];
 uint64_t old_deadline=idle_candidate.deadline_ns;clock_ticks+=1000;
 unsigned before_send=verify_send_calls,before_recv=verify_recv_calls;
 CHECK(!sf_persist_candidate_discard(token));
 SfPersistIdleStatus idle=verify_report(token);s=caps_report(token);
 CHECK(idle.phase==SF_PERSIST_IDLE_CLOSED&&idle.discard_accepted&&idle.death_confirmed&&idle.retired&&
       idle.discard_id>staged1&&idle.deadline_ns>old_deadline&&idle.pid==13);
 CHECK(s.phase==SF_PERSIST_CAPS_RETIRED&&s.branches_retired&&!s.parent[0]&&!s.parent[1]&&
       s.revoke_result[0]==0&&s.revoke_result[1]==0&&candidate_usage==2);
 CHECK(revoked_parent[0]==0x601&&revoked_parent[1]==0x602&&
       verify_send_calls==before_send+1&&verify_recv_calls==before_recv+1);
 CHECK(candidate_caps.request_id[0]==staged0&&candidate_caps.request_id[1]==staged1);
 stable_after_cycle(token);cases++;
}

static void busy_and_capacity_refusal_are_retryable(void){
 uint64_t token=prepare_hold_caps();
 unsigned dirty=ledger.modules[fs_slot].resources[RES_OPEN];
 uint64_t before_nonce=requests;
 unsigned d=derive_calls,se=stage_send_calls,re=stage_recv_calls,rv=branch_revoke_calls;
 ledger.modules[fs_slot].resources[RES_OPEN]++;
 CHECK(sf_persist_candidate_stage_caps(token)==SF_BUSY);
 CHECK(requests==before_nonce&&derive_calls==d&&stage_send_calls==se&&stage_recv_calls==re&&
       branch_revoke_calls==rv&&candidate_caps.phase==SF_PERSIST_CAPS_NONE&&candidate_usage==2);
 ledger.modules[fs_slot].resources[RES_OPEN]=dirty;
 CHECK(!sf_persist_candidate_stage_caps(token));CHECK(candidate_caps.phase==SF_PERSIST_CAPS_READY);
 CHECK(!sf_persist_candidate_discard(token));stable_after_cycle(token);cases++;

 for(unsigned fail_index=0;fail_index<2;fail_index++){
  token=prepare_hold_caps();before_nonce=requests;reset_caps_hooks();caps_hook_enabled=1;
  derive_result[fail_index]=SF_NO_SPACE;d=derive_calls;se=stage_send_calls;re=stage_recv_calls;rv=branch_revoke_calls;
  CHECK(sf_persist_candidate_stage_caps(token)==SF_NO_SPACE);
  CHECK(requests==before_nonce&&derive_calls==d+fail_index+1&&stage_send_calls==se&&stage_recv_calls==re&&
        branch_revoke_calls==rv&&candidate_caps.phase==SF_PERSIST_CAPS_NONE&&
        candidate_caps.parent[0]==0&&candidate_usage==2);
  if(fail_index)CHECK(close_calls==1&&closed_parent[0]==0x601);
  reset_caps_hooks();caps_hook_enabled=1;
  CHECK(!sf_persist_candidate_stage_caps(token));CHECK(candidate_caps.phase==SF_PERSIST_CAPS_READY);
  CHECK(!sf_persist_candidate_discard(token));stable_after_cycle(token);cases++;
 }
 token=prepare_hold_caps();reset_caps_hooks();caps_hook_enabled=1;
 derive_result[1]=SF_NO_SPACE;cap_close_error=SF_DENIED;
 CHECK(sf_persist_candidate_stage_caps(token)==SF_NO_SPACE);
 CHECK(candidate_caps.phase==SF_PERSIST_CAPS_FAILED&&close_calls==1&&closed_parent[0]==0x601&&
       branch_revoke_calls==1&&revoked_parent[0]==0x601&&candidate_caps.branches_retired&&
       !candidate_caps.parent[0]&&candidate_usage==2&&!stage_send_calls);
 assert_no_stage_retry(token,SF_DRAINING);stable_after_cycle(token);cases++;
}

static void preflight_and_derive_fail_closed(void){
 uint64_t token=prepare_hold_caps();
 uint64_t nonce=requests;unsigned ep=verify_ep_calls,sp=verify_spawn_calls;
 CHECK(sf_persist_candidate_stage_caps(token+1)==SF_STALE_HANDLE);
 CHECK(requests==nonce&&verify_ep_calls==ep&&verify_spawn_calls==sp&&!derive_calls&&!stage_send_calls);

 token=prepare_hold_caps();candidate_dead_at=1;
 CHECK(sf_persist_candidate_stage_caps(token)==SF_DEVICE_GONE);
 CHECK(candidate_caps.phase==SF_PERSIST_CAPS_FAILED&&!derive_calls&&!stage_send_calls&&
       !branch_revoke_calls&&candidate_caps.branches_retired);
 candidate_dead_at=0;assert_no_stage_retry(token,SF_DRAINING);stable_after_cycle(token);cases++;

 token=prepare_hold_caps();driver_pid++;
 CHECK(sf_persist_candidate_stage_caps(token)==SF_STALE_HANDLE);
 CHECK(candidate_caps.phase==SF_PERSIST_CAPS_FAILED&&!derive_calls&&!stage_send_calls&&!branch_revoke_calls);
 driver_pid--;assert_no_stage_retry(token,SF_DRAINING);stable_after_cycle(token);cases++;

 token=prepare_hold_caps();candidate_usage=3;
 CHECK(sf_persist_candidate_stage_caps(token)==SF_BAD_FORMAT);
 CHECK(candidate_caps.phase==SF_PERSIST_CAPS_FAILED&&!derive_calls&&!stage_send_calls&&!branch_revoke_calls);
 assert_no_stage_retry(token,SF_DRAINING);stable_after_cycle(token);cases++;

 token=prepare_hold_caps();requests=UINT64_MAX-2;nonce=requests;
 CHECK(sf_persist_candidate_stage_caps(token)==SF_NO_SPACE);
 CHECK(requests==nonce&&!candidate_caps.token&&!derive_calls&&!stage_send_calls&&
       idle_candidate.phase==SF_PERSIST_IDLE_PREPARED);
 requests=0x300000100ull;CHECK(!sf_persist_candidate_stage_caps(token));
 CHECK(!sf_persist_candidate_discard(token));stable_after_cycle(token);cases++;

 token=prepare_hold_caps();clock_fault=SF_TIMEOUT;clock_fault_at=clock_calls+1;
 CHECK(sf_persist_candidate_stage_caps(token)==SF_BAD_FORMAT);
 CHECK(candidate_caps.phase==SF_PERSIST_CAPS_FAILED&&!derive_calls&&!stage_send_calls);
 clock_fault=0;assert_no_stage_retry(token,SF_DRAINING);stable_after_cycle(token);cases++;

 int64_t bad_results[]={SF_DENIED,0,SF_VERSION_REJECTED-1,2147483648ll,0x201,0x202,0x301,0x302};
 for(unsigned i=0;i<sizeof(bad_results)/sizeof(*bad_results);i++){
  token=prepare_hold_caps();reset_caps_hooks();caps_hook_enabled=1;
  derive_result[0]=bad_results[i];
  CHECK(sf_persist_candidate_stage_caps(token)==(bad_results[i]<0&&bad_results[i]>=SF_VERSION_REJECTED?
                                                 (int)bad_results[i]:SF_BAD_FORMAT));
  SfPersistCapsStatus s=caps_report(token);
  CHECK(s.phase==SF_PERSIST_CAPS_FAILED&&s.derive_result[0]==bad_results[i]&&
        s.allocation_unknown==(bad_results[i]>0)&&!stage_send_calls&&
        s.parent[0]==0&&candidate_usage==2);
  CHECK(!branch_revoke_calls&&driver_ep==0x201&&driver_reply==0x202&&persist_ep==0&&persist_reply==0);
  assert_no_stage_retry(token,SF_DRAINING);stable_after_cycle(token);cases++;
 }
}

static void ack_and_transport_failures_retire_only_staged_branches(void){
 uint64_t token;
 const unsigned mutations[]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18};
 for(unsigned i=0;i<sizeof(mutations)/sizeof(mutations[0]);i++){
  uint64_t token=prepare_hold_caps();stage_bad_ack=mutations[i];stage_bad_ack_at=1;
  int expected=mutations[i]==17?SF_STALE_HANDLE:SF_BAD_FORMAT;
  CHECK(sf_persist_candidate_stage_caps(token)==expected);
  SfPersistCapsStatus s=caps_report(token);SfPersistIdleStatus idle=verify_report(token);
  CHECK(s.phase==SF_PERSIST_CAPS_FAILED&&s.branches_retired&&!s.parent[0]&&!s.parent[1]&&
        s.revoke_result[0]==0&&s.revoke_result[1]==0&&candidate_usage==2&&
        stage_send_calls==1&&stage_recv_calls==1&&branch_revoke_calls==2&&
        idle.phase==SF_PERSIST_IDLE_FAILED);
  CHECK((mutations[i]==7)==(closes==1));
  assert_no_stage_retry(token,SF_DRAINING);stable_after_cycle(token);cases++;
 }

 for(unsigned stage=1;stage<=2;stage++){
  uint64_t token=prepare_hold_caps();stage_bad_ack=10;stage_bad_ack_at=stage;
  CHECK(sf_persist_candidate_stage_caps(token)==SF_BAD_FORMAT);
  SfPersistCapsStatus s=caps_report(token);
  CHECK(s.phase==SF_PERSIST_CAPS_FAILED&&s.branches_retired&&candidate_usage==2&&
        stage_send_calls==stage&&branch_revoke_calls==2);
  assert_no_stage_retry(token,SF_DRAINING);stable_after_cycle(token);cases++;
 }

 for(unsigned stage=1;stage<=2;stage++){
  uint64_t token=prepare_hold_caps();stage_fail_send=stage;stage_send_error=SF_TIMEOUT;
  stage_deliver_on_failed_send=1;
  CHECK(sf_persist_candidate_stage_caps(token)==SF_TIMEOUT);
  SfPersistCapsStatus s=caps_report(token);
  CHECK(s.phase==SF_PERSIST_CAPS_FAILED&&s.branches_retired&&!s.parent[0]&&!s.parent[1]&&
        candidate_usage==2&&stage_send_calls==stage&&stage_recv_calls==stage-1&&branch_revoke_calls==2);
  assert_no_stage_retry(token,SF_DRAINING);stable_after_cycle(token);cases++;
 }

 for(unsigned stage=1;stage<=2;stage++){
  uint64_t token=prepare_hold_caps();stage_fail_recv=stage;stage_recv_error=SF_DEVICE_GONE;
  CHECK(sf_persist_candidate_stage_caps(token)==SF_DEVICE_GONE);
  SfPersistCapsStatus s=caps_report(token);
  CHECK(s.phase==SF_PERSIST_CAPS_FAILED&&s.branches_retired&&!s.parent[0]&&!s.parent[1]&&
        candidate_usage==2&&stage_send_calls==stage&&stage_recv_calls==stage&&branch_revoke_calls==2);
  assert_no_stage_retry(token,SF_DRAINING);stable_after_cycle(token);cases++;
 }

 token=prepare_hold_caps();stage_peer_status=SF_BUSY;
 CHECK(sf_persist_candidate_stage_caps(token)==SF_BUSY);
 CHECK(caps_report(token).phase==SF_PERSIST_CAPS_FAILED&&candidate_usage==2&&
       branch_revoke_calls==2&&stage_send_calls==1&&stage_recv_calls==1);
 assert_no_stage_retry(token,SF_DRAINING);stable_after_cycle(token);cases++;

 token=prepare_hold_caps();stage_bad_from=12;
 CHECK(sf_persist_candidate_stage_caps(token)==SF_BAD_FORMAT);
 CHECK(caps_report(token).phase==SF_PERSIST_CAPS_FAILED&&stage_send_calls==1&&
       stage_recv_calls==1&&branch_revoke_calls==2&&candidate_usage==2);
 assert_no_stage_retry(token,SF_DRAINING);stable_after_cycle(token);cases++;
}

static void revocation_failures_remain_explicit(void){
 uint64_t token=prepare_hold_caps();CHECK(!sf_persist_candidate_stage_caps(token));
 branch_revoke_error[0]=SF_DENIED;clock_ticks+=1000;
 CHECK(sf_persist_candidate_discard(token)==SF_DENIED);
 SfPersistCapsStatus caps=caps_report(token);SfPersistIdleStatus idle=verify_report(token);
 CHECK(caps.phase==SF_PERSIST_CAPS_FAILED&&!caps.branches_retired&&caps.parent[0]==0x601&&
       !caps.parent[1]&&caps.revoke_result[0]==SF_DENIED&&caps.revoke_result[1]==0);
 CHECK(idle.phase==SF_PERSIST_IDLE_FAILED&&idle.death_confirmed&&idle.retired&&candidate_usage==3);
 CHECK(branch_revoke_calls==2&&revoked_parent[0]==0x601&&revoked_parent[1]==0x602);
 CHECK(sf_persist_candidate_discard(token)==SF_DRAINING&&branch_revoke_calls==2);
 stable_after_cycle(token);cases++;
}

static void staged_discard_failures_revoke_parents_once(void){
 for(unsigned kind=0;kind<5;kind++){
  uint64_t token=prepare_hold_caps();CHECK(!sf_persist_candidate_stage_caps(token));
  unsigned sent=stage_send_calls,received=stage_recv_calls,revokes=branch_revoke_calls;
  uint64_t nonce=requests;
  if(kind==0){fail_recv_opcode=OP_PERSIST_CANDIDATE_DISCARD;recv_error=SF_TIMEOUT;}
  else if(kind==1)verify_discard_bad_field=BAD_DEADLINE;
  else if(kind==2)requests=UINT64_MAX;
  else if(kind==3){clock_fault=SF_TIMEOUT;clock_fault_at=clock_calls+1;}
  else {fail_recv_opcode=OP_PERSIST_CANDIDATE_DISCARD;recv_error=SF_TIMEOUT;branch_revoke_error[0]=SF_DENIED;}

  int expected=kind==1?SF_BAD_FORMAT:kind==2?SF_NO_SPACE:kind==3?SF_BAD_FORMAT:SF_TIMEOUT;
  CHECK(sf_persist_candidate_discard(token)==expected);
  SfPersistCapsStatus caps=caps_report(token);SfPersistIdleStatus idle=verify_report(token);
  CHECK(caps.phase==SF_PERSIST_CAPS_FAILED&&caps.error==expected&&
        caps.parent[0]==(kind==4?0x601:0)&&caps.parent[1]==0&&
        caps.branches_retired==(kind!=4)&&caps.revoke_result[0]==(kind==4?SF_DENIED:0)&&
        caps.revoke_result[1]==0);
  CHECK(idle.phase==SF_PERSIST_IDLE_FAILED&&!idle.death_confirmed&&!idle.retired&&
        stage_send_calls==sent&&stage_recv_calls==received&&branch_revoke_calls==revokes+2&&
        revoked_parent[revokes]==0x601&&revoked_parent[revokes+1]==0x602);
  if(kind<2||kind==4)CHECK(verify_send_calls>0&&verify_last_sent.h.opcode==OP_PERSIST_CANDIDATE_DISCARD);
  else CHECK(requests==(kind==2?UINT64_MAX:nonce)&&verify_last_sent.h.opcode!=OP_PERSIST_CANDIDATE_DISCARD);
  CHECK(sf_persist_candidate_discard(token)==SF_DRAINING&&branch_revoke_calls==revokes+2&&
        stage_send_calls==sent&&stage_recv_calls==received);
  clock_fault=0;fail_recv_opcode=fail_recv_at=verify_discard_bad_field=0;
  if(kind==2)requests=nonce;
  stable_after_cycle(token);cases++;
 }
}

int main(void){
 normal_stage_and_discard();
 busy_and_capacity_refusal_are_retryable();preflight_and_derive_fail_closed();
 ack_and_transport_failures_retire_only_staged_branches();revocation_failures_remain_explicit();
 staged_discard_failures_revoke_parents_once();
 reset();printf("PERSIST CAPS OWNER PASS: %u checks/%u cases; E1 mocked transport and synthetic snapshot\n",checks,cases);
 return 0;
}
