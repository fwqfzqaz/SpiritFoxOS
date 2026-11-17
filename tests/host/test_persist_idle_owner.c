/* E1: actual Root candidate-cycle/status owner with mocked kernel, spawn, and
 * transport. It does not execute a guest or establish package signatures. */
#include <limits.h>
#define main terminal_baseline_main
#define SF_TEST_IDLE_OWNER
#include "test_persist_terminal_owner.c"
#undef SF_TEST_IDLE_OWNER
#undef main
#include "../../services/persist_candidate.h"

static int idle_hook_enabled;
static unsigned idle_ep_calls,idle_send_calls,idle_recv_calls,idle_revoke_calls,idle_task_calls;
static unsigned idle_spawn_calls;
static int idle_ep_fail_at,idle_reply_variant,idle_task_abnormal,idle_task_wide;
static unsigned idle_task_running,idle_mutate_at,idle_mutate_kind;
static int64_t idle_ep_result[2],idle_send_result,idle_recv_result,idle_from,idle_spawn_result,idle_revoke_error;
static int64_t idle_reply_status,idle_guard_status,idle_guard_recv_result;
static int64_t idle_old_status_after_spawn;
static SfMessage idle_sent;

static int idle_owner_sys_hook(long op,uint64_t a,uint64_t b,uint64_t c,int64_t*out){
 (void)b;(void)c;if(!idle_hook_enabled)return 0;
 if(op==SYS_EP_CREATE){
  unsigned index=idle_ep_calls++;
  CHECK(index<2);
  if(idle_ep_fail_at==(int)(index+1))*out=idle_ep_result[index];
  else *out=(int64_t)(0x301u+index);
  if(idle_mutate_at==5&&index==1)mutate(idle_mutate_kind);
  return 1;
 }
 if(op==SYS_TASK_STATUS&&a==idle_candidate.pid&&idle_candidate.pid){
  idle_task_calls++;
  if(idle_task_running){idle_task_running--;*out=SF_TASK_RUNNING;}
  else if(idle_task_wide)*out=(1ll<<32)+1;
  else if(idle_task_abnormal)*out=SF_DENIED;
  else *out=0;
  if(idle_mutate_at==4&&*out==0)mutate(idle_mutate_kind);
  return 1;
 }
 if(op==SYS_TASK_STATUS&&a==12&&idle_candidate.pid&&idle_old_status_after_spawn){
  *out=idle_old_status_after_spawn;return 1;
 }
 if(op==SYS_CAP_REVOKE&&(a==0x301||a==0x302)){
  CHECK((!idle_candidate.pid&&!idle_candidate.probe_accepted&&!idle_candidate.discard_accepted)||
        (idle_candidate.probe_accepted&&idle_candidate.guard_denied&&idle_candidate.discard_accepted&&idle_candidate.death_confirmed&&idle_candidate.task_status==0));
  idle_revoke_calls++;*out=idle_revoke_error;return 1;
 }
 return 0;
}
static int idle_owner_send_hook(uint32_t ep,SfMessage*m,int64_t*out){
 if(!idle_hook_enabled||ep!=idle_candidate.ep)return 0;
 idle_send_calls++;idle_sent=*m;
 CHECK(m->h.size==sizeof(SfIpcHeader)&&m->h.protocol_major==1&&!m->h.protocol_minor);
 uint64_t expected_id=m->h.opcode==OP_PERSIST_CANDIDATE_PROBE?idle_candidate.probe_id:
                      m->h.opcode==OP_PERSIST_NOTES_SIZE?idle_candidate.guard_id:idle_candidate.discard_id;
 CHECK((m->h.opcode==OP_PERSIST_CANDIDATE_PROBE||m->h.opcode==OP_PERSIST_NOTES_SIZE||
        m->h.opcode==OP_PERSIST_CANDIDATE_DISCARD)&&!m->h.flags&&m->h.request_id==expected_id);
 CHECK(m->h.payload_bytes==sizeof(*m)-sizeof(SfIpcHeader)&&!m->h.cap_count&&
       m->h.deadline_ns==idle_candidate.deadline_ns&&!m->status&&!m->handle&&!m->offset&&!m->length&&!m->mode);
 for(unsigned i=0;i<sizeof(m->data);i++)CHECK(!m->data[i]);
 *out=idle_send_result;return 1;
}
static int idle_owner_recv_hook(uint32_t ep,SfMessage*m,uint64_t deadline,int64_t*out){
 if(!idle_hook_enabled||ep!=idle_candidate.reply)return 0;
 idle_recv_calls++;CHECK(deadline==idle_candidate.deadline_ns);
 if(idle_sent.h.opcode==OP_PERSIST_NOTES_SIZE&&idle_guard_recv_result){*out=idle_guard_recv_result;return 1;}
 if(idle_recv_result){*out=idle_recv_result;return 1;}
 *m=idle_sent;m->h.flags=idle_from? (uint32_t)idle_from:idle_candidate.pid;
 m->status=idle_sent.h.opcode==OP_PERSIST_NOTES_SIZE?idle_guard_status:idle_reply_status;
 m->mode=m->status==0?1u:0u;
 *out=idle_from?idle_from:idle_candidate.pid;
 switch(idle_reply_variant){
  case 1:m->h.size--;break;case 2:m->h.protocol_major++;break;case 3:m->h.protocol_minor++;break;
  case 4:m->h.opcode++;break;case 5:m->h.flags++;break;case 6:m->h.request_id++;break;
  case 7:m->h.payload_bytes--;break;case 8:m->h.deadline_ns++;break;
  case 9:m->h.cap_count=1;m->handle=0x777;break;case 10:m->h.cap_count=2;break;
  case 11:m->handle=1;break;case 12:m->offset=1;break;case 13:m->length=1;break;
  case 14:m->mode^=1u;break;case 15:m->status=1;break;case 16:m->status=-(1ll<<32);break;
  case 17:m->data[0]=1;break;case 18:*out=(1ll<<32)+idle_candidate.pid;break;
 }
 if(idle_mutate_at==1&&idle_recv_calls==1)mutate(idle_mutate_kind);
 if(idle_mutate_at==2&&idle_recv_calls==2)mutate(idle_mutate_kind);
 if(idle_mutate_at==3&&idle_recv_calls==3)mutate(idle_mutate_kind);
 return 1;
}

static int64_t idle_spawn(const SfRuntimeTrust*trust,uint32_t ep,uint32_t reply,void*context){
 idle_spawn_calls++;
 CHECK(trust==&terminal_stop.trust&&ep==0x302&&reply==0x301&&context==(void*)0x1);
 return idle_spawn_result;
}
static uint64_t start_idle(void){
 idle_hook_enabled=0;
 reset();uint64_t token=begin();CHECK(!sf_persist_fence_stop(token));snapshot_state();
 memset(&idle_candidate,0,sizeof(idle_candidate));idle_hook_enabled=1;
 idle_ep_calls=idle_send_calls=idle_recv_calls=idle_revoke_calls=idle_task_calls=idle_spawn_calls=0;
 idle_ep_fail_at=idle_reply_variant=idle_task_abnormal=idle_task_wide=0;
 idle_task_running=idle_mutate_at=idle_mutate_kind=0;
 idle_ep_result[0]=idle_ep_result[1]=SF_DEVICE_GONE;
 idle_send_result=idle_recv_result=idle_revoke_error=idle_reply_status=idle_guard_status=idle_guard_recv_result=0;
 idle_guard_status=SF_DRAINING;
 idle_old_status_after_spawn=0;
 idle_from=0;idle_spawn_result=13;memset(&idle_sent,0,sizeof(idle_sent));
 return token;
}
static SfPersistIdleStatus idle_report(uint64_t token){
 SfPersistIdleStatus s;memset(&s,0xa5,sizeof(s));CHECK(!sf_persist_candidate_status(token,&s));CHECK(s.token==token);return s;
}
static void idle_no_retry_expected(uint64_t token,int expected){
 unsigned ep=idle_ep_calls,sends0=idle_send_calls,recvs=idle_recv_calls,spawns=idle_spawn_calls;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==expected);
 CHECK(ep==idle_ep_calls&&sends0==idle_send_calls&&recvs==idle_recv_calls&&spawns==idle_spawn_calls);
}
static void idle_no_retry(uint64_t token){idle_no_retry_expected(token,SF_DRAINING);}

static void success_and_status(void){
 uint64_t token=start_idle();idle_task_running=3;unsigned old_revoke=revokes,old_send=sends,old_recv=receives;
 SfPersistStopStatus stop_before=report(token);
 CHECK(!sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1));
 SfPersistIdleStatus s=idle_report(token);
 CHECK(s.phase==SF_PERSIST_IDLE_CLOSED&&s.token==token&&s.pid==13&&s.ep==0x302&&s.reply==0x301);
 CHECK(s.probe_accepted&&s.guard_denied&&s.discard_accepted&&s.death_confirmed&&s.retired&&s.observations==4&&!s.error);
 CHECK(s.probe_id<s.guard_id&&s.guard_id<s.discard_id&&s.deadline_ns&&s.spawn_result==13&&s.probe_send==0&&s.probe_receive==13);
 CHECK(s.guard_send==0&&s.guard_receive==13&&s.discard_send==0&&s.discard_receive==13);
 CHECK(!s.task_status&&!s.revoke_ep_result&&!s.revoke_reply_result);
 CHECK(idle_ep_calls==2&&idle_send_calls==3&&idle_recv_calls==3&&idle_revoke_calls==2&&idle_task_calls==4);
 CHECK(revokes==old_revoke&&sends==old_send&&receives==old_recv&&idle_spawn_calls==1);
 unsigned ep_count=idle_ep_calls,send_count=idle_send_calls,recv_count=idle_recv_calls,revoke_count=idle_revoke_calls,task_count=idle_task_calls;
 SfPersistIdleStatus again=idle_report(token);CHECK(!memcmp(&s,&again,sizeof(s)));
 SfPersistStopStatus stop_after=report(token);CHECK(!memcmp(&stop_before,&stop_after,sizeof(stop_before)));
 CHECK(ep_count==idle_ep_calls&&send_count==idle_send_calls&&recv_count==idle_recv_calls&&
       revoke_count==idle_revoke_calls&&task_count==idle_task_calls);
 SfPersistIdleStatus untouched;memset(&untouched,0xa5,sizeof(untouched));SfPersistIdleStatus before=untouched;
 CHECK(sf_persist_candidate_status(token+1,&untouched)==SF_STALE_HANDLE&&!memcmp(&before,&untouched,sizeof(before)));
 CHECK(sf_persist_candidate_status(token,0)==SF_INVALID);
 unchanged();CHECK(persist_pid==12&&!persist_ep&&!persist_reply&&sf_persist_fenced());idle_no_retry(token);cases++;
}

static void allocation_and_spawn_failures(void){
 uint64_t token=start_idle();idle_ep_fail_at=1;idle_ep_result[0]=SF_NO_SPACE;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_NO_SPACE);
 SfPersistIdleStatus s=idle_report(token);CHECK(s.phase==SF_PERSIST_IDLE_FAILED&&s.error==SF_NO_SPACE&&!s.ep&&!s.reply&&!s.retired);
 CHECK(!idle_revoke_calls&&!idle_send_calls);idle_no_retry(token);cases++;

 token=start_idle();idle_ep_fail_at=2;idle_ep_result[1]=SF_DEVICE_GONE;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_DEVICE_GONE);
 s=idle_report(token);CHECK(s.phase==SF_PERSIST_IDLE_FAILED&&s.reply==0x301&&!s.ep&&s.retired);
 CHECK(idle_revoke_calls==1);idle_no_retry(token);cases++;

 token=start_idle();idle_ep_fail_at=2;idle_ep_result[1]=SF_DEVICE_GONE;idle_revoke_error=SF_DENIED;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_DENIED);
 s=idle_report(token);CHECK(s.phase==SF_PERSIST_IDLE_FAILED&&s.revoke_reply_result==SF_DENIED&&!s.retired);
 CHECK(idle_revoke_calls==1);idle_no_retry(token);cases++;

 token=start_idle();idle_spawn_result=SF_DENIED;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_DENIED);
 s=idle_report(token);CHECK(s.phase==SF_PERSIST_IDLE_FAILED&&s.spawn_result==SF_DENIED&&s.retired);
 CHECK(idle_revoke_calls==2&&!idle_send_calls);idle_no_retry(token);cases++;

 const int64_t bad_spawn[]={1,12,(1ll<<32)+13};
 for(unsigned i=0;i<sizeof(bad_spawn)/sizeof(bad_spawn[0]);i++){
  token=start_idle();idle_spawn_result=bad_spawn[i];
  CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_BAD_FORMAT);
  s=idle_report(token);CHECK(s.phase==SF_PERSIST_IDLE_FAILED&&s.spawn_result==bad_spawn[i]);
  /* Invalid positive spawn identities are not safe evidence for endpoint
   * cleanup; current owner retains capabilities and prohibits another cycle. */
  CHECK(!s.retired&&!idle_revoke_calls&&!idle_send_calls);idle_no_retry(token);cases++;
 }
}

static void exchange_failures(void){
 for(int variant0=1;variant0<=18;variant0++){
  uint64_t token=start_idle();idle_reply_variant=variant0;
  CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_BAD_FORMAT);
  SfPersistIdleStatus s=idle_report(token);CHECK(s.phase==SF_PERSIST_IDLE_FAILED&&!s.probe_accepted&&s.error==SF_BAD_FORMAT);
  CHECK(idle_send_calls==1&&idle_recv_calls==1&&!idle_revoke_calls);
  CHECK(closes==(variant0==9?1u:0u));idle_no_retry(token);cases++;
 }
 const int64_t recv_codes[]={SF_TIMEOUT,SF_WOULD_BLOCK,SF_DEVICE_GONE,1,(1ll<<32)+13};
 for(unsigned i=0;i<sizeof(recv_codes)/sizeof(recv_codes[0]);i++){
  uint64_t token=start_idle();idle_recv_result=recv_codes[i];
  int expected=recv_codes[i]<0?(int)recv_codes[i]:SF_BAD_FORMAT;
  CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==expected);
  SfPersistIdleStatus s=idle_report(token);CHECK(s.phase==SF_PERSIST_IDLE_FAILED&&s.probe_receive==recv_codes[i]);
  CHECK(!idle_revoke_calls);idle_no_retry(token);cases++;
 }
 uint64_t token=start_idle();idle_send_result=SF_TIMEOUT;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_TIMEOUT);
 CHECK(idle_report(token).phase==SF_PERSIST_IDLE_FAILED&&idle_recv_calls==0&&!idle_revoke_calls);idle_no_retry(token);cases++;
 token=start_idle();idle_reply_status=SF_BUSY;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_BUSY);
 CHECK(idle_report(token).probe_receive==13&&!idle_revoke_calls);idle_no_retry(token);cases++;

 token=start_idle();idle_guard_status=SF_BUSY;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_BUSY);
 SfPersistIdleStatus guard=idle_report(token);CHECK(guard.phase==SF_PERSIST_IDLE_FAILED&&guard.probe_accepted&&!guard.guard_denied&&!guard.discard_accepted);
 CHECK(guard.guard_receive==13&&idle_send_calls==2&&idle_recv_calls==2&&!idle_revoke_calls);idle_no_retry(token);cases++;
 token=start_idle();idle_guard_recv_result=SF_DRAINING;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_DRAINING);
 guard=idle_report(token);CHECK(guard.phase==SF_PERSIST_IDLE_FAILED&&guard.guard_receive==SF_DRAINING&&!guard.guard_denied);
 CHECK(idle_send_calls==2&&idle_recv_calls==2&&!idle_revoke_calls);idle_no_retry(token);cases++;

 token=start_idle();idle_from=14;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_BAD_FORMAT);
 CHECK(idle_report(token).probe_receive==14&&!idle_revoke_calls);idle_no_retry(token);cases++;
}

static void death_and_context_failures(void){
 uint64_t token=start_idle();idle_task_abnormal=1;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_DEVICE_GONE);
 SfPersistIdleStatus s=idle_report(token);CHECK(s.phase==SF_PERSIST_IDLE_FAILED&&s.probe_accepted&&s.discard_accepted&&!s.death_confirmed&&!s.retired&&!idle_revoke_calls);
 idle_no_retry(token);cases++;
 token=start_idle();idle_task_wide=1;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_DEVICE_GONE);
 CHECK(idle_report(token).task_status==((1ll<<32)+1)&&!idle_revoke_calls);idle_no_retry(token);cases++;
 token=start_idle();idle_task_running=10000;freeze_clock=1;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_TIMEOUT);
 s=idle_report(token);CHECK(s.phase==SF_PERSIST_IDLE_FAILED&&s.observations==512&&!s.death_confirmed&&!idle_revoke_calls);
 idle_no_retry(token);cases++;
 token=start_idle();idle_task_running=10000;
 CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_TIMEOUT);
 s=idle_report(token);CHECK(s.phase==SF_PERSIST_IDLE_FAILED&&s.observations==301&&s.deadline_ns==3100000000ull&&!idle_revoke_calls);
 idle_no_retry(token);cases++;

 const unsigned points[]={1,2,3,4,5};
 for(unsigned i=0;i<sizeof(points)/sizeof(points[0]);i++){
  token=start_idle();idle_mutate_at=points[i];idle_mutate_kind=points[i]==1?5:points[i]==2?20:points[i]==3?21:points[i]==4?5:20;
  int expected=idle_mutate_kind==20?SF_COMMIT_UNCERTAIN:SF_STALE_HANDLE;
  CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==expected);
  s=idle_report(token);CHECK(s.phase==SF_PERSIST_IDLE_FAILED);
  CHECK(s.retired==(points[i]==5));
  CHECK(idle_revoke_calls==(points[i]==5?2u:0u));
  idle_no_retry_expected(token,expected);cases++;
 }
}

static int forbidden_probe(const SfRuntimeTrust*t,SfPersistCandidateImage*out,void*c){
 (void)t;(void)out;(void)c;CHECK(0);return SF_INVALID;
}
static void old_record_reuse(void){
 uint64_t token=start_idle();idle_old_status_after_spawn=SF_NOT_FOUND;
 CHECK(!sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1));
 CHECK(idle_report(token).phase==SF_PERSIST_IDLE_CLOSED&&idle_revoke_calls==2);
 SfPersistCandidateImage image;memset(&image,0xa5,sizeof(image));
 CHECK(sf_persist_fence_inspect(token,forbidden_probe,0,&image)==SF_DEVICE_GONE);
 SfPersistCandidateImage zero;memset(&zero,0,sizeof(zero));CHECK(!memcmp(&image,&zero,sizeof(zero)));
 idle_no_retry_expected(token,SF_DEVICE_GONE);cases++;
 const int64_t bad_old[]={SF_TASK_RUNNING,SF_DENIED,((1ll<<32)+1)};
 for(unsigned i=0;i<sizeof(bad_old)/sizeof(*bad_old);i++){
  token=start_idle();idle_old_status_after_spawn=bad_old[i];
  CHECK(sf_persist_candidate_cycle(token,idle_spawn,(void*)0x1)==SF_DEVICE_GONE);
  CHECK(idle_report(token).phase==SF_PERSIST_IDLE_FAILED&&!idle_send_calls&&!idle_revoke_calls);
  idle_no_retry_expected(token,SF_DEVICE_GONE);cases++;
 }
}
int main(void){
 success_and_status();allocation_and_spawn_failures();exchange_failures();death_and_context_failures();
 old_record_reuse();
 printf("PERSIST IDLE OWNER PASS %u checks; E1 actual Root owner with mocked spawn/kernel/transport; no signature or guest-execution claim\n",checks);
 return 0;
}
