/* E1 only: actual Root ownership checks with a trusted mock candidate probe.
 * Kernel identity, transport, the probe, and its load-plan data are mocked;
 * this is not package-signature or guest-execution evidence. */
#define main terminal_baseline_main
#include "test_persist_terminal_owner.c"
#undef main
#include "../../services/persist_candidate.h"

static unsigned probe_calls,probe_shape,probe_context_mutation;
static int probe_result;

static int candidate_probe(const SfRuntimeTrust *trust,SfPersistCandidateImage *out,void *context){
 CHECK(trust!=0&&context==(void*)0xcafe);
 probe_calls++;
 memset(out,0,sizeof(*out));out->bytes=4096;
 out->source_sha256[0]=0x31;out->key_id[0]=0x52;
 out->plan.size=sizeof(out->plan);out->plan.count=1;out->plan.image_base=SF_BASE;
 if(probe_shape==1)out->plan.grant_manager=1;
 if(probe_shape==2)out->plan.grant_service=1;
 if(probe_shape==3)out->plan.service_rights=1;
 if(probe_shape==4)out->plan.grant_device=1;
 if(probe_shape==5)out->plan.device_rights=1;
 if(probe_shape==6)out->plan.grant_bundle=1;
 if(probe_shape==7)out->plan.arguments.data[0]=1;
 if(probe_shape==8)out->plan.size--;
 if(probe_shape==9)out->plan.count=0;
 if(probe_shape==10)out->plan.count=9;
 if(probe_shape==11)out->plan.image_base=SF_PIE_MIN;
 if(probe_shape==12)out->bytes=0;
 if(probe_shape==13)out->bytes=SF_PACKAGE_MAX+1ull;
 if(probe_context_mutation)mutate(probe_context_mutation);
 return probe_result;
}

static void output_zero(const SfPersistCandidateImage *out){
 SfPersistCandidateImage zero;memset(&zero,0,sizeof(zero));
 CHECK(!memcmp(out,&zero,sizeof(zero)));
}
static void denied_probe(uint64_t token,int expected){
 SfPersistCandidateImage out;memset(&out,0xa5,sizeof(out));probe_calls=0;
 int r=sf_persist_fence_inspect(token,candidate_probe,(void*)0xcafe,&out);
 CHECK(expected?r==expected:r<0);
 CHECK(probe_calls==0);output_zero(&out);
}
static uint64_t stopped(void){
 reset();uint64_t token=begin();CHECK(!sf_persist_fence_stop(token));
 CHECK(terminal_stop.phase==SF_PERSIST_TERMINAL_STOPPED_CLOSED);return token;
}
static void valid_inspection_tests(void){
 uint64_t token=stopped();snapshot_state();SfPersistStopStatus before=report(token);
 unsigned old_send=sends,old_recv=receives,old_revoke=revokes;
 for(unsigned i=0;i<3;i++){
  SfPersistCandidateImage out;memset(&out,0xa5,sizeof(out));probe_calls=0;
  CHECK(!sf_persist_fence_inspect(token,candidate_probe,(void*)0xcafe,&out));
  CHECK(probe_calls==1&&out.bytes==4096&&out.source_sha256[0]==0x31&&out.key_id[0]==0x52);
  CHECK(out.plan.size==sizeof(out.plan)&&out.plan.count==1&&out.plan.image_base==SF_BASE);
  CHECK(!out.plan.grant_manager&&!out.plan.grant_service&&!out.plan.service_rights&&
        !out.plan.grant_device&&!out.plan.device_rights&&!out.plan.grant_bundle);
  SfArguments empty;memset(&empty,0,sizeof(empty));CHECK(!memcmp(&out.plan.arguments,&empty,sizeof(empty)));
  SfPersistStopStatus after=report(token);CHECK(!memcmp(&before,&after,sizeof(before)));
  unchanged();
 }
 CHECK(sends==old_send&&receives==old_recv&&revokes==old_revoke);
 denied_probe(0,SF_STALE_HANDLE);denied_probe(token+1,SF_STALE_HANDLE);
 cases++;
}
static void phase_denial_tests(void){
 for(unsigned phase=0;phase<5;phase++){
  reset();uint64_t token=begin();
  int expected=SF_DRAINING;
  switch(phase){
   case 0:CHECK(terminal_stop.phase==SF_PERSIST_TERMINAL_ACTIVE);break;
   case 1:
    freeze_clock=1;final_status=SF_TASK_RUNNING;send_result=SF_TIMEOUT;
    CHECK(sf_persist_fence_stop(token)==SF_TIMEOUT);
    CHECK(terminal_stop.phase==SF_PERSIST_TERMINAL_REQUEST_UNKNOWN);break;
   case 2:
    freeze_clock=1;final_status=SF_TASK_RUNNING;semantic=0;
    CHECK(sf_persist_fence_stop(token)==SF_TIMEOUT);
    CHECK(terminal_stop.phase==SF_PERSIST_TERMINAL_ACCEPTED_WAIT);break;
   case 3:
    semantic=SF_COMMIT_UNCERTAIN;final_status=SF_TASK_RUNNING;
    CHECK(sf_persist_fence_stop(token)==SF_COMMIT_UNCERTAIN);
    CHECK(terminal_stop.phase==SF_PERSIST_TERMINAL_FAILED);break;
   case 4:
    semantic=SF_BUSY;final_status=SF_TASK_RUNNING;
    CHECK(sf_persist_fence_stop(token)==SF_BUSY);
    CHECK(terminal_stop.phase==SF_PERSIST_TERMINAL_REJECTED);break;
  }
  denied_probe(token,expected);cases++;
 }
}
static void stop_context_denial_tests(void){
 for(unsigned fault=0;fault<10;fault++){
  uint64_t token=stopped();
  switch(fault){
   case 0:terminal_stop.ack_authenticated=0;break;
   case 1:terminal_stop.death_confirmed=0;break;
   case 2:terminal_stop.retired=0;break;
   case 3:terminal_stop.task_status=1;break;
   case 4:terminal_stop.revoke_ep_result=SF_DENIED;break;
   case 5:terminal_stop.revoke_reply_result=SF_DENIED;break;
   case 6:terminal_stop.pid++;break;
   case 7:terminal_stop.trust.revoked_ids[0][0]++;break;
   case 8:terminal_stop.driver_pid++;break;
   case 9:terminal_stop.fs_pid++;break;
  }
  denied_probe(token,SF_STALE_HANDLE);cases++;
 }
 for(unsigned mutation_kind=1;mutation_kind<24+RES_COUNT;mutation_kind++){
  uint64_t token=stopped();mutate(mutation_kind);
  denied_probe(token,0);cases++;
 }
}
static void probe_failure_tests(void){
 uint64_t token=stopped();
 probe_shape=0;probe_context_mutation=0;probe_result=SF_BUSY;
 SfPersistCandidateImage out;memset(&out,0xa5,sizeof(out));probe_calls=0;
 CHECK(sf_persist_fence_inspect(token,candidate_probe,(void*)0xcafe,&out)==SF_BUSY);
 CHECK(probe_calls==1);output_zero(&out);cases++;
 probe_result=1;memset(&out,0xa5,sizeof(out));probe_calls=0;
 CHECK(sf_persist_fence_inspect(token,candidate_probe,(void*)0xcafe,&out)==SF_BAD_FORMAT);
 CHECK(probe_calls==1);output_zero(&out);cases++;
 probe_result=0;
 for(probe_shape=1;probe_shape<=13;probe_shape++){
  memset(&out,0xa5,sizeof(out));probe_calls=0;
  CHECK(sf_persist_fence_inspect(token,candidate_probe,(void*)0xcafe,&out)==SF_BAD_FORMAT);
  CHECK(probe_calls==1);output_zero(&out);cases++;
 }
 probe_shape=0;
 for(unsigned mutation_kind=5;mutation_kind<=21;mutation_kind+=16){
  reset();token=stopped();probe_context_mutation=mutation_kind;probe_calls=0;
  memset(&out,0xa5,sizeof(out));
  CHECK(sf_persist_fence_inspect(token,candidate_probe,(void*)0xcafe,&out)==SF_STALE_HANDLE);
  CHECK(probe_calls==1);output_zero(&out);cases++;
 }
}
static void null_and_token_tests(void){
 uint64_t token=stopped();SfPersistCandidateImage out;memset(&out,0xa5,sizeof(out));
 CHECK(sf_persist_fence_inspect(token,0,(void*)0xcafe,&out)==SF_INVALID);output_zero(&out);
 CHECK(sf_persist_fence_inspect(token,candidate_probe,(void*)0xcafe,0)==SF_INVALID);
 probe_calls=0;memset(&out,0xa5,sizeof(out));
 CHECK(sf_persist_fence_inspect(0,candidate_probe,(void*)0xcafe,&out)==SF_STALE_HANDLE);
 CHECK(!probe_calls);output_zero(&out);cases++;
}
int main(void){
 valid_inspection_tests();phase_denial_tests();stop_context_denial_tests();probe_failure_tests();null_and_token_tests();
 printf("PERSIST CANDIDATE OWNER PASS %u checks; %u cases; E1 actual Root owner with mock probe/kernel/transport; no package-signature or guest-proof claim\n",checks,cases);
 return 0;
}
