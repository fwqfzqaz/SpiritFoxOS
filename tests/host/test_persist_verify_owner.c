/* E1: actual Root verify-cycle owner with mocked endpoint allocation, spawn,
 * task status, and IPC. A synthetic captured snapshot and sectors exercise
 * protocol flow only; this is not snapshot acquisition, transaction, guest,
 * signature, or disk evidence. */
#include <limits.h>
#define main terminal_baseline_main
#define SF_TEST_IDLE_OWNER
#include "test_persist_terminal_owner.c"
#undef SF_TEST_IDLE_OWNER
#undef main
#include "../../services/persist_candidate.h"

static int verify_hook_enabled;
static unsigned verify_ep_calls,verify_send_calls,verify_recv_calls;
static unsigned verify_spawn_calls,verify_revoke_calls,verify_task_calls;
static unsigned frame_target,read_replies,frame_index,death_running,death_status_calls;
static unsigned bad_stage,bad_field,callback_mutate,callback_mutate_kind;
static int64_t spawn_result,send_error,recv_error,recv_from,clock_fault,read_error;
static unsigned clock_fault_at,clock_calls,candidate_dead_at,old_not_found_after_spawn;
static unsigned fail_send_opcode,fail_send_at,fail_recv_opcode,fail_recv_at;
static uint64_t verify_lba_start;
static SfMessage verify_last_sent;
static SfPersistSnapshot synthetic_capture;
static SfPersistCaptureStatus capture_before;
static SfPersistStopStatus stop_before;
static unsigned old_send_count,old_recv_count,old_revoke_count;

#define OWNER_CHECK(c) CHECK(c)
enum {
 BAD_NONE,BAD_SIZE,BAD_MAJOR,BAD_MINOR,BAD_OPCODE,BAD_FLAGS,BAD_NONCE,BAD_CAPS,
 BAD_PAYLOAD,BAD_DEADLINE,BAD_MODE,BAD_LBA,BAD_SEQUENCE,BAD_LENGTH,BAD_STATUS,
 BAD_HANDLE,BAD_TAIL,BAD_FROM,BAD_SNAPSHOT,BAD_CAPS_WIDE
};

static void fill_snapshot(SfPersistSnapshot*s){
 memset(s,0,sizeof(*s));s->size=sizeof(*s);s->version=1;s->generation=23;s->notes_bytes=17;
 for(unsigned i=0;i<16;i++)s->volume[i]=(uint8_t)(0x20+i);
 for(unsigned i=0;i<32;i++){s->files_sha256[i]=(uint8_t)(0x40+i);s->trust_sha256[i]=(uint8_t)(0x80+i);}
}
static void sector_pattern(uint64_t lba,uint8_t out[512]){
 for(unsigned i=0;i<512;i++)out[i]=(uint8_t)(lba*31u+i*17u+(lba>>2));
}
static void apply_bad(SfMessage*m,unsigned field,unsigned final){
 switch(field){
  case BAD_SIZE:m->h.size--;break;case BAD_MAJOR:m->h.protocol_major++;break;
  case BAD_MINOR:m->h.protocol_minor++;break;case BAD_OPCODE:m->h.opcode++;break;
  case BAD_FLAGS:m->h.flags++;break;case BAD_NONCE:m->h.request_id++;break;
  case BAD_CAPS:m->h.cap_count=1;m->handle=0x777;break;
  case BAD_CAPS_WIDE:m->h.cap_count=2;break;
  case BAD_PAYLOAD:m->h.payload_bytes--;break;case BAD_DEADLINE:m->h.deadline_ns++;break;
  case BAD_MODE:m->mode=final?1u:0u;break;case BAD_LBA:m->offset=final?1u:SF_TXN_BANK+2*SF_TXN_BANK_SECTORS;break;
  case BAD_SEQUENCE:m->handle++;break;case BAD_LENGTH:m->length--;break;
  case BAD_STATUS:m->status=1;break;case BAD_HANDLE:m->handle=1;break;
  case BAD_TAIL:m->data[final?sizeof(SfPersistSnapshot):512]=1;break;
  case BAD_FROM:break;
  case BAD_SNAPSHOT:((SfPersistSnapshot*)m->data)->generation++;break;
  default:break;
 }
}
static void make_ack(SfMessage*m,int status){
 *m=verify_last_sent;m->h.flags=13;m->status=status;m->mode=status?0u:1u;
 m->handle=m->offset=m->length=0;memset(m->data,0,sizeof(m->data));
}
static void make_read_frame(SfMessage*m,unsigned index){
 *m=verify_last_sent;m->h.flags=13;m->mode=1;m->status=0;
 m->offset=verify_lba_start+index;m->handle=index+1;m->length=512;memset(m->data,0,sizeof(m->data));
 if(bad_stage==1&&bad_field)apply_bad(m,bad_field,0);
}
static void make_final_frame(SfMessage*m,int status){
 *m=verify_last_sent;m->h.flags=13;m->mode=0;m->status=status;
 m->handle=m->offset=0;memset(m->data,0,sizeof(m->data));
 if(!status){m->length=sizeof(synthetic_capture);memcpy(m->data,&synthetic_capture,sizeof(synthetic_capture));}
 else m->length=0;
 if(bad_stage==2&&bad_field)apply_bad(m,bad_field,1);
}

static int idle_owner_sys_hook(long op,uint64_t a,uint64_t b,uint64_t c,int64_t*out){
 (void)b;(void)c;if(!verify_hook_enabled)return 0;
 if(op==SYS_TICKS){clock_calls++;if(clock_fault&&clock_calls==clock_fault_at){*out=clock_fault;return 1;}*out=(int64_t)clock_ticks;return 1;}
 if(op==SYS_YIELD){yields++;if(!freeze_clock)clock_ticks++;*out=0;return 1;}
 if(op==SYS_EP_CREATE){unsigned ix=verify_ep_calls++;*out=ix==0?0x301:0x302;return 1;}
 if(op==SYS_TASK_STATUS&&a==12){
  verify_task_calls++;*out=old_not_found_after_spawn&&verify_spawn_calls?SF_NOT_FOUND:0;return 1;
 }
 if(op==SYS_TASK_STATUS&&a==13){
  verify_task_calls++;
  if(candidate_dead_at&&verify_recv_calls>=candidate_dead_at){*out=0;return 1;}
  if(verify_last_sent.h.opcode==OP_PERSIST_CANDIDATE_DISCARD||
     (verify_send_calls&&verify_last_sent.h.opcode==OP_PERSIST_CANDIDATE_DISCARD)){
   death_status_calls++;if(death_running){death_running--;*out=SF_TASK_RUNNING;}else *out=0;
   return 1;
  }
  *out=SF_TASK_RUNNING;return 1;
 }
 if(op==SYS_CAP_REVOKE&&(a==0x301||a==0x302)){
  OWNER_CHECK(idle_candidate.verified&&idle_candidate.death_confirmed&&idle_candidate.discard_accepted);
  verify_revoke_calls++;*out=0;return 1;
 }
 return 0;
}

static int idle_owner_send_hook(uint32_t ep,SfMessage*m,int64_t*out){
 if(!verify_hook_enabled||ep!=0x302)return 0;
 verify_send_calls++;verify_last_sent=*m;
 OWNER_CHECK(m->h.size==sizeof(SfIpcHeader)&&m->h.protocol_major==1&&!m->h.protocol_minor&&
            (m->h.opcode==OP_PERSIST_NOTES_SIZE||
             (m->h.opcode>OP_PERSIST_STOP&&m->h.opcode<=OP_PERSIST_CANDIDATE_VERIFY))&&
            !m->h.flags&&m->h.payload_bytes==sizeof(*m)-sizeof(SfIpcHeader)&&!m->h.cap_count);
 if(m->h.opcode==OP_PERSIST_CANDIDATE_PROBE||m->h.opcode==OP_PERSIST_NOTES_SIZE||
    m->h.opcode==OP_PERSIST_CANDIDATE_DISCARD){
  OWNER_CHECK(!m->status&&!m->handle&&!m->offset&&!m->length&&!m->mode);
  for(unsigned i=0;i<sizeof(m->data);i++)OWNER_CHECK(!m->data[i]);
 }
 if(m->h.opcode==OP_PERSIST_CANDIDATE_VERIFY&&m->mode==0&&m->length==sizeof(SfPersistSnapshot)){
  OWNER_CHECK(m->h.request_id==idle_candidate.verify_id&&
              m->h.deadline_ns==idle_candidate.verify_deadline_ns&&
              !memcmp(m->data,&synthetic_capture,sizeof(synthetic_capture)));
  for(unsigned i=sizeof(synthetic_capture);i<sizeof(m->data);i++)OWNER_CHECK(!m->data[i]);
 }
 if(m->h.opcode==OP_PERSIST_CANDIDATE_VERIFY&&m->mode==1&&m->length==512){
  uint8_t expected[512];sector_pattern(m->offset,expected);
  OWNER_CHECK(m->h.request_id==idle_candidate.verify_id&&m->h.deadline_ns==idle_candidate.verify_deadline_ns&&
              m->handle==m->offset-verify_lba_start+1&&m->status==0&&!memcmp(m->data,expected,512));
  read_replies++;
 }
 if(fail_send_opcode==m->h.opcode&&(!fail_send_at||verify_send_calls==fail_send_at)){
  *out=send_error;return 1;
 }
 *out=0;return 1;
}

static int idle_owner_recv_hook(uint32_t ep,SfMessage*m,uint64_t deadline,int64_t*out){
 if(!verify_hook_enabled||ep!=0x301)return 0;
 verify_recv_calls++;
 OWNER_CHECK(deadline==verify_last_sent.h.deadline_ns);
 if(fail_recv_opcode==verify_last_sent.h.opcode&&(!fail_recv_at||verify_recv_calls==fail_recv_at)){
  *out=recv_error;return 1;
 }
 unsigned op=verify_last_sent.h.opcode;
 if(op==OP_PERSIST_CANDIDATE_PROBE){make_ack(m,0);*out=recv_from?recv_from:13;return 1;}
 if(op==OP_PERSIST_NOTES_SIZE){make_ack(m,SF_DRAINING);*out=recv_from?recv_from:13;return 1;}
 if(op==OP_PERSIST_CANDIDATE_DISCARD){make_ack(m,0);*out=recv_from?recv_from:13;return 1;}
 OWNER_CHECK(op==OP_PERSIST_CANDIDATE_VERIFY);
 if(verify_last_sent.mode==0&&verify_last_sent.length==sizeof(SfPersistSnapshot)){
  frame_index=0;make_read_frame(m,frame_index++);*out=bad_stage==1&&bad_field==BAD_FROM?(1ll<<32)+13:recv_from?recv_from:13;return 1;
 }
 if(verify_last_sent.mode==1&&verify_last_sent.status<0&&!verify_last_sent.length){
  make_final_frame(m,(int)verify_last_sent.status);*out=recv_from?recv_from:13;return 1;
 }
 OWNER_CHECK(verify_last_sent.mode==1&&verify_last_sent.length==512);
 if(verify_last_sent.status){make_final_frame(m,(int)verify_last_sent.status);*out=recv_from?recv_from:13;return 1;}
 if(read_replies<frame_target){make_read_frame(m,frame_index++);*out=bad_stage==1&&bad_field==BAD_FROM?(1ll<<32)+13:recv_from?recv_from:13;return 1;}
 make_final_frame(m,0);*out=bad_stage==2&&bad_field==BAD_FROM?(1ll<<32)+13:recv_from?recv_from:13;return 1;
}

static int64_t verify_spawn(const SfRuntimeTrust*trust,uint32_t ep,uint32_t reply,void*context){
 verify_spawn_calls++;
 OWNER_CHECK(trust==&terminal_stop.trust&&ep==0x302&&reply==0x301&&context==(void*)0x1234);
 return spawn_result;
}
static int verify_sector_read(void*context,uint64_t lba,uint8_t out[512]){
 (void)context;if(lba<SF_TXN_HEADER||lba>=SF_TXN_BANK+2*SF_TXN_BANK_SECTORS)return SF_INVALID;
 if(callback_mutate){callback_mutate=0;mutate(callback_mutate_kind);}
 if(read_error)return read_error;
 sector_pattern(lba,out);return 0;
}

static uint64_t start_owner(unsigned frames){
 verify_hook_enabled=0;reset();uint64_t token=begin();CHECK(!sf_persist_fence_stop(token));snapshot_state();
 memset(&fence_capture,0,sizeof(fence_capture));fill_snapshot(&synthetic_capture);
 fence_capture.token=token;fence_capture.pid=12;fence_capture.phase=SF_PERSIST_CAPTURE_CAPTURED;
 fence_capture.authenticated=1;fence_capture.snapshot=synthetic_capture;
 memset(&idle_candidate,0,sizeof(idle_candidate));
 verify_ep_calls=verify_send_calls=verify_recv_calls=verify_spawn_calls=verify_revoke_calls=verify_task_calls=0;
 frame_target=frames;read_replies=frame_index=death_status_calls=0;death_running=2;
 bad_stage=bad_field=callback_mutate=callback_mutate_kind=0;
 spawn_result=13;send_error=recv_error=recv_from=clock_fault=read_error=0;clock_fault_at=clock_calls=0;
 candidate_dead_at=old_not_found_after_spawn=0;fail_send_opcode=fail_send_at=fail_recv_opcode=fail_recv_at=0;
 verify_lba_start=SF_TXN_HEADER;memset(&verify_last_sent,0,sizeof(verify_last_sent));
 capture_before=fence_capture;stop_before=report(token);
 old_send_count=sends;old_recv_count=receives;old_revoke_count=revokes;
 verify_hook_enabled=1;return token;
}
static SfPersistIdleStatus verify_report(uint64_t token){
 SfPersistIdleStatus s;memset(&s,0xa5,sizeof(s));CHECK(!sf_persist_candidate_status(token,&s)&&s.token==token);return s;
}
static void stable_after_cycle(uint64_t token){
 SfPersistStopStatus stop_after=report(token);SfPersistCaptureStatus capture_after;
 CHECK(!sf_persist_fence_capture_status(token,&capture_after));
 CHECK(!memcmp(&stop_before,&stop_after,sizeof(stop_before))&&!memcmp(&capture_before,&capture_after,sizeof(capture_before)));
 CHECK(sends==old_send_count&&receives==old_recv_count&&revokes==old_revoke_count);
 CHECK(!uncertain_commit&&!trust_error&&!ledger.modules[fs_slot].resources[RES_DIRTY]);
 unchanged();
}
static void verify_no_retry(uint64_t token){
 unsigned ep=verify_ep_calls,sp=verify_spawn_calls,se=verify_send_calls,re=verify_recv_calls,rv=verify_revoke_calls;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==
       (old_not_found_after_spawn?SF_DEVICE_GONE:SF_DRAINING));
 CHECK(ep==verify_ep_calls&&sp==verify_spawn_calls&&se==verify_send_calls&&re==verify_recv_calls&&rv==verify_revoke_calls);
}
static void no_new_attempt(uint64_t token,int expected){
 unsigned ep=verify_ep_calls,sp=verify_spawn_calls,se=verify_send_calls,re=verify_recv_calls,rv=verify_revoke_calls;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==expected);
 CHECK(ep==verify_ep_calls&&sp==verify_spawn_calls&&se==verify_send_calls&&re==verify_recv_calls&&rv==verify_revoke_calls);
}
static void success_case(unsigned frames,int old_not_found){
 uint64_t token=start_owner(frames);old_not_found_after_spawn=old_not_found;
 CHECK(!sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0));
 SfPersistIdleStatus s=verify_report(token);
 CHECK(s.phase==SF_PERSIST_IDLE_CLOSED&&s.verified&&s.read_frames==frames&&s.verified_snapshot.size==sizeof(SfPersistSnapshot));
 CHECK(!memcmp(&s.verified_snapshot,&synthetic_capture,sizeof(synthetic_capture))&&s.probe_accepted&&s.guard_denied&&
       s.discard_accepted&&s.death_confirmed&&s.retired&&!s.error&&s.pid==13&&s.ep==0x302&&s.reply==0x301);
 CHECK(verify_spawn_calls==1&&verify_ep_calls==2&&verify_revoke_calls==2&&read_replies==frames);
 stable_after_cycle(token);verify_no_retry(token);cases++;
}

static void preconditions(void){
 uint64_t token=start_owner(259);fence_capture.phase=SF_PERSIST_CAPTURE_NONE;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_DRAINING);
 CHECK(!verify_ep_calls&&!verify_spawn_calls&&!verify_send_calls);
 token=start_owner(259);fence_capture.phase=SF_PERSIST_CAPTURE_REFUSED;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_DRAINING);
 CHECK(!verify_ep_calls&&!verify_spawn_calls);
 token=start_owner(259);fence_capture.token++;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_DRAINING);
 CHECK(!verify_ep_calls&&!verify_spawn_calls);
 token=start_owner(259);
 CHECK(sf_persist_candidate_verify_cycle(token+1,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_STALE_HANDLE);
 CHECK(!verify_ep_calls&&!verify_spawn_calls);
 token=start_owner(259);requests=UINT64_MAX-3;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_NO_SPACE);
 CHECK(!verify_ep_calls&&!verify_spawn_calls&&!verify_send_calls);cases+=5;
}

static void frame_mutations(void){
 uint64_t low=start_owner(259);verify_lba_start=SF_TXN_HEADER-1;
 CHECK(sf_persist_candidate_verify_cycle(low,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_BAD_FORMAT);
 CHECK(!verify_report(low).read_frames&&!verify_revoke_calls);stable_after_cycle(low);verify_no_retry(low);cases++;
 const unsigned mutations[]={BAD_SIZE,BAD_MAJOR,BAD_MINOR,BAD_OPCODE,BAD_FLAGS,BAD_NONCE,BAD_CAPS,
  BAD_CAPS_WIDE,BAD_PAYLOAD,BAD_DEADLINE,BAD_MODE,BAD_LBA,BAD_SEQUENCE,BAD_LENGTH,BAD_STATUS,BAD_TAIL,BAD_FROM};
 for(unsigned i=0;i<sizeof(mutations)/sizeof(mutations[0]);i++){
  uint64_t token=start_owner(259);bad_stage=1;bad_field=mutations[i];
  int r=sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0);
  CHECK(r==SF_BAD_FORMAT);SfPersistIdleStatus s=verify_report(token);
  CHECK(s.phase==SF_PERSIST_IDLE_FAILED&&!s.verified&&!s.discard_accepted&&!s.retired);
  CHECK(s.read_frames==0&&verify_spawn_calls==1&&verify_revoke_calls==0);
  CHECK(closes==(mutations[i]==BAD_CAPS?1u:0u));
  stable_after_cycle(token);verify_no_retry(token);cases++;
 }
}

static void final_mutations_and_counts(void){
 const unsigned mutations[]={BAD_SIZE,BAD_MAJOR,BAD_MINOR,BAD_OPCODE,BAD_FLAGS,BAD_NONCE,BAD_CAPS,
  BAD_CAPS_WIDE,BAD_PAYLOAD,BAD_DEADLINE,BAD_MODE,BAD_LENGTH,BAD_STATUS,BAD_HANDLE,BAD_LBA,BAD_TAIL,BAD_SNAPSHOT,BAD_FROM};
 for(unsigned i=0;i<sizeof(mutations)/sizeof(mutations[0]);i++){
  uint64_t token=start_owner(259);bad_stage=2;bad_field=mutations[i];
  int r=sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0);
  CHECK(r==SF_BAD_FORMAT);SfPersistIdleStatus s=verify_report(token);
  CHECK(s.phase==SF_PERSIST_IDLE_FAILED&&!s.verified&&!s.discard_accepted&&!s.retired&&s.read_frames==259);
  CHECK(closes==(mutations[i]==BAD_CAPS?1u:0u));
  CHECK(verify_revoke_calls==0);stable_after_cycle(token);verify_no_retry(token);cases++;
 }
 uint64_t token=start_owner(258);CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_BAD_FORMAT);
 CHECK(verify_report(token).read_frames==258&&!verify_report(token).verified&&!verify_revoke_calls);stable_after_cycle(token);cases++;
 token=start_owner(516);CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_BAD_FORMAT);
 CHECK(verify_report(token).read_frames==515&&!verify_report(token).verified&&!verify_revoke_calls);stable_after_cycle(token);cases++;
}

static void callback_and_transport_failures(void){
 uint64_t token=start_owner(259);callback_mutate=1;callback_mutate_kind=5;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_STALE_HANDLE);
 CHECK(verify_report(token).phase==SF_PERSIST_IDLE_FAILED&&!verify_revoke_calls);no_new_attempt(token,SF_STALE_HANDLE);cases++;
 token=start_owner(259);send_error=SF_TIMEOUT;fail_send_opcode=OP_PERSIST_CANDIDATE_VERIFY;fail_send_at=3;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_TIMEOUT);
 CHECK(!verify_revoke_calls&&verify_report(token).phase==SF_PERSIST_IDLE_FAILED);verify_no_retry(token);cases++;
 token=start_owner(259);send_error=1;fail_send_opcode=OP_PERSIST_CANDIDATE_VERIFY;fail_send_at=3;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_BAD_FORMAT);
 CHECK(!verify_revoke_calls&&verify_report(token).phase==SF_PERSIST_IDLE_FAILED);verify_no_retry(token);cases++;
 token=start_owner(259);send_error=SF_VERSION_REJECTED-1;fail_send_opcode=OP_PERSIST_CANDIDATE_VERIFY;fail_send_at=3;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_BAD_FORMAT);
 CHECK(!verify_revoke_calls&&verify_report(token).phase==SF_PERSIST_IDLE_FAILED);verify_no_retry(token);cases++;
 token=start_owner(259);recv_error=SF_DEVICE_GONE;fail_recv_opcode=OP_PERSIST_CANDIDATE_VERIFY;fail_recv_at=3;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_DEVICE_GONE);
 CHECK(!verify_revoke_calls&&verify_report(token).phase==SF_PERSIST_IDLE_FAILED);verify_no_retry(token);cases++;
 token=start_owner(259);recv_error=1;fail_recv_opcode=OP_PERSIST_CANDIDATE_VERIFY;fail_recv_at=3;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_BAD_FORMAT);
 CHECK(!verify_revoke_calls&&verify_report(token).phase==SF_PERSIST_IDLE_FAILED);verify_no_retry(token);cases++;
 token=start_owner(259);recv_error=SF_VERSION_REJECTED-1;fail_recv_opcode=OP_PERSIST_CANDIDATE_VERIFY;fail_recv_at=3;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_BAD_FORMAT);
 CHECK(!verify_revoke_calls&&verify_report(token).phase==SF_PERSIST_IDLE_FAILED);verify_no_retry(token);cases++;
 token=start_owner(259);read_error=SF_DEVICE_GONE;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_DEVICE_GONE);
 CHECK(!verify_report(token).verified&&!verify_revoke_calls);verify_no_retry(token);cases++;
 token=start_owner(259);read_error=1;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_BAD_FORMAT);
 CHECK(!verify_report(token).verified&&!verify_revoke_calls);verify_no_retry(token);cases++;
 token=start_owner(259);read_error=SF_VERSION_REJECTED-1;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_BAD_FORMAT);
 CHECK(!verify_report(token).verified&&!verify_revoke_calls);verify_no_retry(token);cases++;
 token=start_owner(259);clock_fault=(int64_t)clock_ticks+3000;clock_fault_at=3;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_TIMEOUT);
 CHECK(!verify_revoke_calls&&verify_report(token).phase==SF_PERSIST_IDLE_FAILED);verify_no_retry(token);cases++;
 token=start_owner(259);candidate_dead_at=3;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_DEVICE_GONE);
 CHECK(!verify_revoke_calls&&verify_report(token).phase==SF_PERSIST_IDLE_FAILED);verify_no_retry(token);cases++;
 token=start_owner(259);recv_from=(1ll<<32)+13;
 CHECK(sf_persist_candidate_verify_cycle(token,verify_spawn,(void*)0x1234,verify_sector_read,0)==SF_BAD_FORMAT);
 CHECK(!verify_report(token).verified&&!verify_revoke_calls);verify_no_retry(token);cases++;
}

int main(void){
 success_case(259,0);success_case(515,1);preconditions();frame_mutations();
 final_mutations_and_counts();callback_and_transport_failures();
 printf("PERSIST VERIFY OWNER PASS %u checks across %u cases; mocked kernel/transport and synthetic captured snapshot\n",checks,cases);
 return 0;
}
