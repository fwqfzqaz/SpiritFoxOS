/* E1: Root-side reply-owner integration with mocked kernel/transport and a
 * synthetic snapshot. It makes no real block-driver RPC claim. */
#define SF_CAPS_OWNER_ENTRY caps_owner_baseline_main
#include "test_persist_caps_owner.c"
#undef SF_CAPS_OWNER_ENTRY

static unsigned reply_owner_cases;

typedef struct {
 unsigned sends,receives,closes,verify_send,verify_recv;
 unsigned stage_send,stage_recv,derive,branch_revoke,task_queries,usage_queries,clock_queries;
 uint64_t sequence;
} IoSnapshot;
static IoSnapshot io_snapshot(void){
 return(IoSnapshot){sends,receives,closes,verify_send_calls,verify_recv_calls,
                    stage_send_calls,stage_recv_calls,derive_calls,branch_revoke_calls,
                    verify_task_calls,usage_queries,clock_calls,sequence};
}
static void io_unchanged(IoSnapshot s){
 CHECK(s.sends==sends&&s.receives==receives&&s.closes==closes&&
       s.verify_send==verify_send_calls&&s.verify_recv==verify_recv_calls&&
       s.stage_send==stage_send_calls&&s.stage_recv==stage_recv_calls&&s.derive==derive_calls&&
       s.branch_revoke==branch_revoke_calls&&s.task_queries==verify_task_calls&&
       s.usage_queries==usage_queries&&s.clock_queries==clock_calls&&s.sequence==sequence);
}
static void driver_io_unchanged(IoSnapshot s){
 CHECK(s.sends==sends&&s.receives==receives&&s.closes==closes&&
       s.verify_send==verify_send_calls&&s.verify_recv==verify_recv_calls&&
       s.stage_send==stage_send_calls&&s.stage_recv==stage_recv_calls&&s.sequence==sequence);
}
static SfStorageReplyOwner reply_status(void){
 SfStorageReplyOwner s;memset(&s,0xa5,sizeof(s));CHECK(!sf_storage_reply_status(&s));return s;
}
static void reply_unchanged(const SfStorageReplyOwner*before){
 SfStorageReplyOwner after=reply_status();CHECK(!memcmp(before,&after,sizeof(after)));
}
static void fresh_staged(uint64_t*token,uint64_t*serial){
 memset(&reply_owner,0,sizeof(reply_owner));
 *token=prepare_hold_caps();CHECK(!sf_persist_candidate_stage_caps(*token));
 CHECK(idle_candidate.phase==SF_PERSIST_IDLE_STAGED&&candidate_caps.phase==SF_PERSIST_CAPS_READY&&
       candidate_usage==4&&candidate_caps.parent[0]==0x601&&candidate_caps.parent[1]==0x602);
 *serial=0;
}
static uint64_t borrow(uint64_t token){
 uint64_t serial=UINT64_MAX;CHECK(!sf_persist_candidate_reply_borrow(token,&serial));
 CHECK(serial&&reply_owner.serial==serial&&reply_owner.phase==SF_STORAGE_REPLY_BORROWED&&
       !reply_owner.error&&storage_reply_access(&reply_owner)==SF_DRAINING);
 return serial;
}
static void discard_root(uint64_t token){
 CHECK(!sf_persist_candidate_discard(token));CHECK(idle_candidate.phase==SF_PERSIST_IDLE_CLOSED);
 stable_after_cycle(token);reply_owner_cases++;
}

static void borrow_blocks_root_transport_and_status_is_pure(void){
 uint64_t token,unused;fresh_staged(&token,&unused);uint64_t serial=borrow(token);
 SfStorageReplyOwner history=reply_status();CHECK(history.serial==serial&&history.phase==SF_STORAGE_REPLY_BORROWED&&
       history.seal.candidate_pid==13&&history.seal.driver_ep==0x201&&history.seal.driver_reply==0x202);
 IoSnapshot before=io_snapshot();SfStorageReplyOwner second;memset(&second,0x5a,sizeof(second));
 CHECK(sf_storage_reply_status(&second)==0&&!memcmp(&second,&history,sizeof(second)));
 SfPersistCapsStatus caps_before=candidate_caps;uint64_t out=0xfeedfaceull;
 uint8_t original_trust=sf_trust.revoked_ids[0][0];sf_trust.revoked_ids[0][0]^=0x40;
 CHECK(sf_persist_candidate_reply_borrow(token,&out)==SF_BUSY&&out==0xfeedfaceull);
 sf_trust.revoked_ids[0][0]=original_trust;reply_unchanged(&history);
 io_unchanged(before);
 SfMessage m;sf_message(&m,OP_BLOCK_INFO,0);m.length=0;SfMessage saved=m;uint64_t seq=sequence;
 before=io_snapshot();
 CHECK(call(&m)==SF_DRAINING&&!memcmp(&m,&saved,sizeof(m))&&sequence==seq);
 uint8_t guarded[528];memset(guarded,0xa7,sizeof(guarded));
 CHECK(sector(0,0,guarded+8)==SF_DRAINING);
 for(unsigned i=0;i<sizeof(guarded);i++)CHECK(guarded[i]==0xa7);
 io_unchanged(before);
 CHECK(candidate_caps.phase==SF_PERSIST_CAPS_READY&&candidate_usage==4&&
       candidate_caps.parent[0]==0x601&&candidate_caps.parent[1]==0x602);
 CHECK(!memcmp(&candidate_caps,&caps_before,sizeof(caps_before)));
 SfStorageReplyOwner after=reply_status();CHECK(!memcmp(&history,&after,sizeof(history)));
 CHECK(!sf_persist_candidate_reply_release(token,serial));
 CHECK(reply_owner.phase==SF_STORAGE_REPLY_ROOT&&reply_owner.serial==serial&&
       !memcmp(&reply_owner.seal,&history.seal,sizeof(history.seal))&&
       candidate_caps.phase==SF_PERSIST_CAPS_READY&&idle_candidate.phase==SF_PERSIST_IDLE_STAGED&&
       candidate_usage==4);
 before=io_snapshot();
 CHECK(!sf_persist_candidate_reply_borrow(token,&out)&&out==serial+1);
 driver_io_unchanged(before);
 before=io_snapshot();
 CHECK(sf_persist_candidate_reply_release(token,serial)==SF_STALE_HANDLE);
 io_unchanged(before);
 reply_unchanged(&(SfStorageReplyOwner){.serial=serial+1,.phase=SF_STORAGE_REPLY_BORROWED,
                                       .error=0,.seal=history.seal});
 CHECK(!sf_persist_candidate_reply_release(token,serial+1));
 CHECK(reply_owner.phase==SF_STORAGE_REPLY_ROOT&&reply_owner.serial==serial+1);
 discard_root(token);
}

static void wrong_ids_and_raw_seal_mutations_are_stale(void){
 uint64_t token,unused;fresh_staged(&token,&unused);uint64_t serial=borrow(token);
 SfStorageReplyOwner before=reply_status();IoSnapshot io=io_snapshot();
 SfPersistCapsStatus caps_before=candidate_caps;
 CHECK(sf_persist_candidate_reply_release(0,serial)==SF_STALE_HANDLE);
 CHECK(sf_persist_candidate_reply_release(token+1,serial)==SF_STALE_HANDLE);
 CHECK(sf_persist_candidate_reply_release(token,0)==SF_STALE_HANDLE);
 CHECK(sf_persist_candidate_reply_release(token,serial+1)==SF_STALE_HANDLE);
 CHECK(sf_persist_candidate_reply_quarantine(0,serial,SF_TIMEOUT)==SF_STALE_HANDLE);
 CHECK(sf_persist_candidate_reply_quarantine(token+1,serial,SF_TIMEOUT)==SF_STALE_HANDLE);
 CHECK(sf_persist_candidate_reply_quarantine(token,0,SF_TIMEOUT)==SF_STALE_HANDLE);
 CHECK(sf_persist_candidate_reply_quarantine(token,serial+1,SF_TIMEOUT)==SF_STALE_HANDLE);
 reply_unchanged(&before);io_unchanged(io);
 CHECK(!memcmp(&candidate_caps,&caps_before,sizeof(caps_before)));
 for(unsigned byte=0;byte<sizeof(SfStorageReplySeal);byte++){
  SfStorageReplySeal changed=before.seal;((uint8_t*)&changed)[byte]^=1u;
  CHECK(sf_storage_reply_return_trusted(&changed,serial)==SF_STALE_HANDLE);
  CHECK(sf_storage_reply_taint_trusted(&changed,serial,SF_TIMEOUT)==SF_STALE_HANDLE);
  reply_unchanged(&before);
  CHECK(!memcmp(&candidate_caps,&caps_before,sizeof(caps_before)));
 }
 io_unchanged(io);
 CHECK(!sf_persist_candidate_reply_release(token,serial));discard_root(token);
}

static void invalid_quarantine_and_overflow_do_not_mutate(void){
 uint64_t token,unused;fresh_staged(&token,&unused);uint64_t serial=borrow(token);
 SfStorageReplyOwner before=reply_status();IoSnapshot io=io_snapshot();
 CHECK(sf_persist_candidate_reply_quarantine(token,serial,1)==SF_INVALID);
 CHECK(sf_persist_candidate_reply_quarantine(token,serial,SF_VERSION_REJECTED-1)==SF_INVALID);
 reply_unchanged(&before);io_unchanged(io);
 CHECK(!sf_persist_candidate_reply_release(token,serial));discard_root(token);

 fresh_staged(&token,&unused);reply_owner.serial=UINT64_MAX;before=reply_status();
 uint64_t out=0x87654321ull;io=io_snapshot();
 CHECK(sf_persist_candidate_reply_borrow(token,&out)==SF_NO_SPACE&&out==0x87654321ull);
 reply_unchanged(&before);driver_io_unchanged(io);
 reply_owner.serial=0;discard_root(token);
}

enum {CTX_TRUST,CTX_CAPACITY,CTX_DRIVER_PID,CTX_GENERATION,CTX_CANDIDATE_USAGE,CTX_DRIVER_DEAD,CTX_COUNT};
typedef struct {uint64_t capacity;uint32_t driver_pid,generation;uint8_t trust;} ContextBefore;
static ContextBefore drift_context(unsigned kind){
 ContextBefore b={capacity,driver_pid,ledger.modules[driver_slot].generation,
                  sf_trust.revoked_ids[0][0]};
 switch(kind){
  case CTX_TRUST:sf_trust.revoked_ids[0][0]^=0x40;break;
  case CTX_CAPACITY:capacity+=512;break;
  case CTX_DRIVER_PID:driver_pid++;break;
  case CTX_GENERATION:ledger.modules[driver_slot].generation++;break;
  case CTX_CANDIDATE_USAGE:candidate_usage=5;break;
  case CTX_DRIVER_DEAD:driver_pid=12;break;
 }
 return b;
}
static void restore_context(ContextBefore b){
 capacity=b.capacity;driver_pid=b.driver_pid;ledger.modules[driver_slot].generation=b.generation;
 sf_trust.revoked_ids[0][0]=b.trust;
}
static void live_context_changes_fail_before_borrow(void){
 for(unsigned kind=0;kind<CTX_COUNT;kind++){
  uint64_t token,unused;fresh_staged(&token,&unused);ContextBefore b=drift_context(kind);
  uint64_t out=0xabcddcbaull;SfStorageReplyOwner before=reply_status();
  SfPersistCapsStatus caps_before=candidate_caps;unsigned revokes=branch_revoke_calls;IoSnapshot io=io_snapshot();
  int r=sf_persist_candidate_reply_borrow(token,&out);
  int expected=kind==CTX_CANDIDATE_USAGE?SF_BAD_FORMAT:
               kind==CTX_DRIVER_DEAD?SF_DEVICE_GONE:SF_STALE_HANDLE;
  if(r!=expected)fprintf(stderr,"preborrow drift case %u: got %d expected %d\n",kind,r,expected);
  CHECK(r==expected&&out==0xabcddcbaull);
  reply_unchanged(&before);driver_io_unchanged(io);
  CHECK(reply_owner.phase==SF_STORAGE_REPLY_ROOT&&reply_owner.serial==0&&
       candidate_caps.phase==SF_PERSIST_CAPS_FAILED&&candidate_caps.branches_retired&&
       !candidate_caps.parent[0]&&!candidate_caps.parent[1]&&branch_revoke_calls==revokes+2&&
       revoked_parent[revokes]==0x601&&revoked_parent[revokes+1]==0x602&&
       idle_candidate.phase==SF_PERSIST_IDLE_FAILED&&
       candidate_usage==(kind==CTX_CANDIDATE_USAGE?3u:2u));
  CHECK(caps_before.phase==SF_PERSIST_CAPS_READY);
  restore_context(b);CHECK(storage_reply_access(&reply_owner)==0);
  stable_after_cycle(token);reply_owner_cases++;
 }
}

static void known_busy_and_serial_exhaustion_keep_stage_available(void){
 uint64_t token,unused;fresh_staged(&token,&unused);SfStorageReplyOwner before=reply_status();
 SfPersistCapsStatus caps_before=candidate_caps;unsigned revokes=branch_revoke_calls;
 unsigned opens=ledger.modules[fs_slot].resources[RES_OPEN];
 ledger.modules[fs_slot].resources[RES_OPEN]++;
 uint64_t out=0x12345678ull;IoSnapshot io=io_snapshot();
 CHECK(sf_persist_candidate_reply_borrow(token,&out)==SF_BUSY&&out==0x12345678ull);
 ledger.modules[fs_slot].resources[RES_OPEN]=opens;
 reply_unchanged(&before);driver_io_unchanged(io);
 CHECK(candidate_caps.phase==SF_PERSIST_CAPS_READY&&candidate_usage==4&&
       branch_revoke_calls==revokes&&!memcmp(&candidate_caps,&caps_before,sizeof(caps_before)));
 discard_root(token);

 fresh_staged(&token,&unused);reply_owner.serial=UINT64_MAX;before=reply_status();caps_before=candidate_caps;
 revokes=branch_revoke_calls;out=0x87654321ull;io=io_snapshot();
 CHECK(sf_persist_candidate_reply_borrow(token,&out)==SF_NO_SPACE&&out==0x87654321ull);
 reply_unchanged(&before);driver_io_unchanged(io);
 CHECK(candidate_caps.phase==SF_PERSIST_CAPS_READY&&candidate_usage==4&&branch_revoke_calls==revokes&&
       !memcmp(&candidate_caps,&caps_before,sizeof(caps_before)));
 discard_root(token);
}

static void live_context_changes_during_release_quarantine_and_retire(void){
 for(unsigned kind=0;kind<CTX_COUNT;kind++){
  uint64_t token,unused;fresh_staged(&token,&unused);uint64_t serial=borrow(token);
  ContextBefore b=drift_context(kind);unsigned revokes=branch_revoke_calls;
  IoSnapshot io=io_snapshot();
  int expected=kind==CTX_CANDIDATE_USAGE?SF_BAD_FORMAT:
               kind==CTX_DRIVER_DEAD?SF_DEVICE_GONE:SF_STALE_HANDLE;
  int r=sf_persist_candidate_reply_release(token,serial);
  if(r!=expected)fprintf(stderr,"release drift case %u: got %d expected %d\n",kind,r,expected);
  CHECK(r==expected);
  restore_context(b);
  CHECK(reply_owner.phase==SF_STORAGE_REPLY_TAINTED&&reply_owner.serial==serial&&reply_owner.error==expected&&
        candidate_caps.phase==SF_PERSIST_CAPS_FAILED&&candidate_caps.branches_retired&&
        !candidate_caps.parent[0]&&!candidate_caps.parent[1]&&branch_revoke_calls==revokes+2&&
        revoked_parent[revokes]==0x601&&revoked_parent[revokes+1]==0x602&&
        candidate_usage==(kind==CTX_CANDIDATE_USAGE?3u:2u)&&
        idle_candidate.phase==SF_PERSIST_IDLE_FAILED&&storage_reply_access(&reply_owner)==SF_DRAINING);
  SfStorageReplyOwner tainted=reply_status();CHECK(tainted.phase==SF_STORAGE_REPLY_TAINTED&&tainted.error==expected);
  unsigned once=branch_revoke_calls;CHECK(sf_persist_candidate_reply_borrow(token,(uint64_t[1]){77})==SF_DRAINING);
  CHECK(sf_persist_candidate_reply_release(token,serial)==SF_DRAINING&&branch_revoke_calls==once);
  SfMessage m;sf_message(&m,OP_BLOCK_INFO,0);SfMessage saved=m;uint64_t seq=sequence;
  CHECK(call(&m)==SF_DRAINING&&!memcmp(&m,&saved,sizeof(m))&&sequence==seq);
  uint8_t guarded[512];memset(guarded,0x66,sizeof(guarded));
  CHECK(sector(0,0,guarded)==SF_DRAINING);
  for(unsigned i=0;i<sizeof(guarded);i++)CHECK(guarded[i]==0x66);
  driver_io_unchanged(io);stable_after_cycle(token);reply_owner_cases++;
 }
}

static void explicit_quarantine_is_terminal(void){
 uint64_t token,unused;fresh_staged(&token,&unused);uint64_t serial=borrow(token);
 unsigned revokes=branch_revoke_calls;IoSnapshot before=io_snapshot();
 int quarantine_result=sf_persist_candidate_reply_quarantine(token,serial,SF_TIMEOUT);
 CHECK(quarantine_result==SF_TIMEOUT);
 CHECK(reply_owner.phase==SF_STORAGE_REPLY_TAINTED&&reply_owner.error==SF_TIMEOUT&&reply_owner.serial==serial&&
       candidate_caps.phase==SF_PERSIST_CAPS_FAILED&&candidate_caps.branches_retired&&
       !candidate_caps.parent[0]&&!candidate_caps.parent[1]&&branch_revoke_calls==revokes+2&&
       revoked_parent[revokes]==0x601&&revoked_parent[revokes+1]==0x602&&
       candidate_usage==2&&idle_candidate.phase==SF_PERSIST_IDLE_FAILED);
 CHECK(sf_persist_candidate_reply_borrow(token,(uint64_t[1]){77})==SF_DRAINING);
 CHECK(sf_persist_candidate_reply_release(token,serial)==SF_DRAINING&&
       sf_persist_candidate_reply_quarantine(token,serial,SF_TIMEOUT)==SF_DRAINING&&branch_revoke_calls==revokes+2);
 SfMessage m;sf_message(&m,OP_BLOCK_INFO,0);SfMessage saved=m;uint64_t seq=sequence;
 CHECK(call(&m)==SF_DRAINING&&!memcmp(&m,&saved,sizeof(m))&&sequence==seq);
 uint8_t guarded[512];memset(guarded,0x5c,sizeof(guarded));
 CHECK(sector(0,0,guarded)==SF_DRAINING);
 for(unsigned i=0;i<sizeof(guarded);i++)CHECK(guarded[i]==0x5c);
 driver_io_unchanged(before);stable_after_cycle(token);reply_owner_cases++;
}

int main(void){
 borrow_blocks_root_transport_and_status_is_pure();wrong_ids_and_raw_seal_mutations_are_stale();
 invalid_quarantine_and_overflow_do_not_mutate();live_context_changes_fail_before_borrow();
 known_busy_and_serial_exhaustion_keep_stage_available();
 live_context_changes_during_release_quarantine_and_retire();explicit_quarantine_is_terminal();
 reset();printf("PERSIST REPLY OWNER PASS: %u checks/%u cases; E1 mocked context and transport\n",checks,reply_owner_cases);
 return 0;
}
