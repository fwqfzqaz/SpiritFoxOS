/* E1: actual Root hold/discard owner reusing the verify-owner mocked transport.
 * The snapshot and sectors remain synthetic: no guest or real-volume claim. */
#define SF_VERIFY_OWNER_ENTRY verify_owner_baseline_main
#include "test_persist_verify_owner.c"
#undef SF_VERIFY_OWNER_ENTRY

static unsigned hold_checks,hold_cases;
#define HCHECK(c) do{hold_checks++;CHECK(c);}while(0)

static uint64_t prepare_hold_frames(unsigned frames,int forget){
 uint64_t token=start_owner(frames);old_not_found_after_spawn=forget;
 HCHECK(!sf_persist_candidate_hold(token,verify_spawn,(void*)0x1234,verify_sector_read,0));
 SfPersistIdleStatus s=verify_report(token);
 HCHECK(s.phase==SF_PERSIST_IDLE_PREPARED&&s.verified&&s.read_frames==frames&&s.guard_denied);
 HCHECK(!s.discard_accepted&&!s.death_confirmed&&!s.retired&&!verify_revoke_calls);
 HCHECK(verify_spawn_calls==1&&verify_ep_calls==2&&verify_send_calls==frames+3&&verify_recv_calls==frames+3);
 stable_after_cycle(token);return token;
}
static uint64_t prepare_hold(void){return prepare_hold_frames(259,0);}
static void full_volume_and_forgotten_record(void){
 uint64_t token=prepare_hold_frames(515,1);clock_ticks+=1000;
 HCHECK(!sf_persist_candidate_discard(token));SfPersistIdleStatus s=verify_report(token);
 HCHECK(s.phase==SF_PERSIST_IDLE_CLOSED&&s.read_frames==515&&s.retired&&verify_revoke_calls==2);
 stable_after_cycle(token);hold_cases++;
}
static void no_private_work(uint64_t token,int expected){
 unsigned ep=verify_ep_calls,sp=verify_spawn_calls,se=verify_send_calls,re=verify_recv_calls,rv=verify_revoke_calls;
 HCHECK(sf_persist_candidate_hold(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==expected);
 HCHECK(ep==verify_ep_calls&&sp==verify_spawn_calls&&se==verify_send_calls&&re==verify_recv_calls&&rv==verify_revoke_calls);
}
static void prepared_reads_stay_closed(void){
 uint64_t token=prepare_hold();SfPersistIdleStatus s=verify_report(token);
 unsigned task=verify_task_calls,ep=verify_ep_calls,se=verify_send_calls,re=verify_recv_calls,rv=verify_revoke_calls;
 for(unsigned i=0;i<8;i++){SfPersistIdleStatus again=verify_report(token);HCHECK(!memcmp(&s,&again,sizeof(s)));}
 HCHECK(task==verify_task_calls&&ep==verify_ep_calls&&se==verify_send_calls&&re==verify_recv_calls&&rv==verify_revoke_calls);
 HCHECK(sf_persist_fenced()&&sf_persist_fence_cancel(token)==SF_DRAINING&&sf_persist_fence_stop(token)==SF_DRAINING);
 uint64_t denied=0xfeed;HCHECK(sf_persist_load_begin(&denied)==SF_DRAINING&&denied==0xfeed);
 HCHECK(sf_storage_transaction(1)==SF_DRAINING&&sf_storage_unmount()==SF_DRAINING);
 HCHECK(sf_persist_quiescent()==SF_DEVICE_GONE&&verify_send_calls==se&&verify_recv_calls==re);
 no_private_work(token,SF_DRAINING);stable_after_cycle(token);hold_cases++;
}
static void refreshed_discard_succeeds(void){
 uint64_t token=prepare_hold();uint64_t reserved=idle_candidate.discard_id,old_deadline=idle_candidate.deadline_ns;
 uint64_t old_ticks=clock_ticks;clock_ticks+=1000;uint64_t issued_ticks=clock_ticks;
 unsigned before_send=verify_send_calls,before_recv=verify_recv_calls;
 HCHECK(!sf_persist_candidate_discard(token));SfPersistIdleStatus s=verify_report(token);
 HCHECK(s.phase==SF_PERSIST_IDLE_CLOSED&&s.discard_accepted&&s.death_confirmed&&s.retired&&s.observations==3);
 HCHECK(s.discard_id>reserved&&s.deadline_ns>old_deadline&&s.deadline_ns==issued_ticks*10000000ull+3000000000ull);
 HCHECK(verify_last_sent.h.opcode==OP_PERSIST_CANDIDATE_DISCARD&&
        verify_last_sent.h.request_id==s.discard_id&&verify_last_sent.h.deadline_ns==s.deadline_ns);
 HCHECK(verify_send_calls==before_send+1&&verify_recv_calls==before_recv+1&&verify_revoke_calls==2);
 HCHECK(clock_ticks>old_ticks);stable_after_cycle(token);hold_cases++;
}
static void busy_discard_can_resume(void){
 uint64_t token=prepare_hold(),reserved=idle_candidate.discard_id;
 unsigned sent=verify_send_calls,received=verify_recv_calls,spawned=verify_spawn_calls,eps=verify_ep_calls;
 uint64_t nonce=requests;
 /* Model a known live storage resource; this is not a cleared DIRTY/trust state. */
 ledger.modules[fs_slot].resources[RES_OPEN]++;
 HCHECK(sf_persist_candidate_discard(token)==SF_BUSY);
 HCHECK(idle_candidate.phase==SF_PERSIST_IDLE_PREPARED&&idle_candidate.discard_id==reserved&&
        requests==nonce&&verify_send_calls==sent&&verify_recv_calls==received&&
        verify_spawn_calls==spawned&&verify_ep_calls==eps&&!verify_revoke_calls);
 ledger.modules[fs_slot].resources[RES_OPEN]--;
 HCHECK(!sf_persist_candidate_discard(token));
 SfPersistIdleStatus s=verify_report(token);
 HCHECK(s.phase==SF_PERSIST_IDLE_CLOSED&&s.discard_accepted&&s.death_confirmed&&s.retired&&
        s.discard_id>reserved&&requests==nonce+1&&verify_revoke_calls==2);
 stable_after_cycle(token);hold_cases++;
}
static void invalid_entry_and_nonce(void){
 uint64_t token=start_owner(259);unsigned ep=verify_ep_calls,sp=verify_spawn_calls;
 HCHECK(sf_persist_candidate_discard(token)==SF_STALE_HANDLE);
 HCHECK(!verify_ep_calls&&!verify_spawn_calls);
 fence_capture.phase=SF_PERSIST_CAPTURE_REFUSED;
 HCHECK(sf_persist_candidate_hold(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_DRAINING);
 HCHECK(verify_ep_calls==ep&&verify_spawn_calls==sp&&!verify_send_calls);
 token=start_owner(259);HCHECK(sf_persist_candidate_hold(token+1,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_STALE_HANDLE);
 HCHECK(!verify_ep_calls&&!verify_spawn_calls&&!verify_send_calls);
 token=prepare_hold();requests=UINT64_MAX;unsigned sent=verify_send_calls,received=verify_recv_calls;
 HCHECK(sf_persist_candidate_discard(token)==SF_NO_SPACE);
 HCHECK(idle_candidate.phase==SF_PERSIST_IDLE_PREPARED&&!idle_candidate.discard_accepted&&
        verify_send_calls==sent&&verify_recv_calls==received&&!verify_revoke_calls);
 stable_after_cycle(token);hold_cases+=4;
}
static void discard_failures_remain_owned(void){
 uint64_t token=prepare_hold();unsigned sent=verify_send_calls;
 candidate_dead_at=1;
 HCHECK(sf_persist_candidate_discard(token)==SF_DEVICE_GONE);
 HCHECK(idle_candidate.phase==SF_PERSIST_IDLE_FAILED&&!idle_candidate.discard_accepted&&
        !idle_candidate.death_confirmed&&!idle_candidate.retired&&verify_send_calls==sent&&!verify_revoke_calls);
 stable_after_cycle(token);no_private_work(token,SF_DRAINING);hold_cases++;

 token=prepare_hold();callback_mutate_kind=5;mutate(callback_mutate_kind);sent=verify_send_calls;
 HCHECK(sf_persist_candidate_discard(token)==SF_STALE_HANDLE);
 HCHECK(idle_candidate.phase==SF_PERSIST_IDLE_FAILED&&!idle_candidate.retired&&verify_send_calls==sent&&!verify_revoke_calls);
 stable_after_cycle(token);no_private_work(token,SF_STALE_HANDLE);hold_cases++;

 token=prepare_hold();send_error=SF_TIMEOUT;fail_send_opcode=OP_PERSIST_CANDIDATE_DISCARD;
 sent=verify_send_calls;unsigned received_before=verify_recv_calls;
 HCHECK(sf_persist_candidate_discard(token)==SF_TIMEOUT);
 HCHECK(idle_candidate.phase==SF_PERSIST_IDLE_FAILED&&!idle_candidate.discard_accepted&&
        verify_send_calls==sent+1&&verify_recv_calls==received_before);
 HCHECK(!verify_revoke_calls);stable_after_cycle(token);no_private_work(token,SF_DRAINING);hold_cases++;

 token=prepare_hold();recv_error=SF_DEVICE_GONE;fail_recv_opcode=OP_PERSIST_CANDIDATE_DISCARD;
 sent=verify_send_calls;unsigned received=verify_recv_calls;
 HCHECK(sf_persist_candidate_discard(token)==SF_DEVICE_GONE);
 HCHECK(idle_candidate.phase==SF_PERSIST_IDLE_FAILED&&!idle_candidate.discard_accepted&&
        verify_send_calls==sent+1&&verify_recv_calls==received+1&&!verify_revoke_calls);
 stable_after_cycle(token);no_private_work(token,SF_DRAINING);hold_cases++;

 token=prepare_hold();verify_discard_bad_field=BAD_SIZE;sent=verify_send_calls;received=verify_recv_calls;
 HCHECK(sf_persist_candidate_discard(token)==SF_BAD_FORMAT);
 HCHECK(idle_candidate.phase==SF_PERSIST_IDLE_FAILED&&!idle_candidate.discard_accepted&&
        verify_send_calls==sent+1&&verify_recv_calls==received+1&&!verify_revoke_calls);
 stable_after_cycle(token);no_private_work(token,SF_DRAINING);hold_cases++;

 token=prepare_hold();clock_fault=SF_TIMEOUT;clock_fault_at=clock_calls+1;sent=verify_send_calls;
 HCHECK(sf_persist_candidate_discard(token)==SF_BAD_FORMAT);
 HCHECK(idle_candidate.phase==SF_PERSIST_IDLE_FAILED&&verify_send_calls==sent&&!verify_revoke_calls);
 stable_after_cycle(token);no_private_work(token,SF_DRAINING);hold_cases++;
}

int main(void){
 unsigned base_checks=checks,base_cases=cases;
 prepared_reads_stay_closed();refreshed_discard_succeeds();busy_discard_can_resume();
 full_volume_and_forgotten_record();
 invalid_entry_and_nonce();discard_failures_remain_owned();
 printf("PERSIST HOLD OWNER PASS %u checks/%u cases; E1 mock transport/kernel and synthetic volume\n",
        checks-base_checks,cases-base_cases+hold_cases);
 return 0;
}
