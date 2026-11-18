/* E1: actual Root capture owner with mocked snapshot transport/kernel. This
 * does not prove the volume digest or real guest/provider behavior. */
#define main terminal_baseline_main
#define SF_TEST_IDLE_OWNER
#include "test_persist_terminal_owner.c"
#undef SF_TEST_IDLE_OWNER
#undef main
#include "../../services/persist_snapshot.h"
#include "../../services/trust_fingerprint.h"

static int capture_hook_enabled,capture_reply_status,capture_reply_variant;
static int capture_send_result,capture_from,reenter_capture,reenter_result;
static int64_t capture_recv_result;
static unsigned capture_mutate_after,capture_mutate_kind;
static unsigned capture_sends,capture_receives;
static SfMessage capture_request;

static void capture_fill_reply(SfMessage*m){
 *m=capture_request;m->h.flags=12;m->status=capture_reply_status;
 m->handle=m->offset=0;m->mode=0;
 if(capture_reply_status){m->length=0;memset(m->data,0,sizeof(m->data));}
 else{
  SfPersistSnapshot snapshot;memset(&snapshot,0,sizeof(snapshot));
  snapshot.size=sizeof(snapshot);snapshot.version=1;snapshot.generation=9;snapshot.notes_bytes=8;
  for(unsigned i=0;i<16;i++)snapshot.volume[i]=(uint8_t)(0x30+i);
  for(unsigned i=0;i<32;i++)snapshot.files_sha256[i]=(uint8_t)(0x50+i);
  CHECK(!sf_trust_fingerprint(&admission_fence.trust,snapshot.trust_sha256));
  m->length=sizeof(snapshot);memset(m->data,0,sizeof(m->data));memcpy(m->data,&snapshot,sizeof(snapshot));
 }
 switch(capture_reply_variant){
  case 1:m->h.flags=13;break;case 2:break;
  case 3:m->h.size--;break;case 4:m->h.protocol_major++;break;case 5:m->h.protocol_minor++;break;
  case 6:m->h.opcode++;break;case 7:m->h.flags++;break;case 8:m->h.request_id++;break;
  case 9:m->h.cap_count=1;m->handle=0x777;break;case 10:m->h.cap_count=2;break;
  case 11:m->h.payload_bytes--;break;case 12:m->h.deadline_ns++;break;
  case 13:m->handle=1;break;case 14:m->offset=1;break;case 15:m->mode=1;break;
  case 16:m->status=1;break;case 17:m->status=SF_VERSION_REJECTED-1;break;
  case 18:m->length--;break;case 19:m->data[sizeof(m->data)-1]=1;break;
  case 20:((SfPersistSnapshot*)m->data)->size--;break;
  case 21:((SfPersistSnapshot*)m->data)->version++;break;
  case 22:((SfPersistSnapshot*)m->data)->generation=0;break;
  case 23:((SfPersistSnapshot*)m->data)->notes_bytes=SF_TXN_FILE_BYTES+1u;break;
  case 24:((SfPersistSnapshot*)m->data)->trust_sha256[0]^=1;break;
  case 25:m->data[sizeof(SfPersistSnapshot)]=1;break;
 }
 if(capture_mutate_after){unsigned point=capture_mutate_after;capture_mutate_after=0;if(point==1)mutate(capture_mutate_kind);}
}
static int idle_owner_sys_hook(long op,uint64_t a,uint64_t b,uint64_t c,int64_t*out){
 (void)op;(void)a;(void)b;(void)c;(void)out;return 0;
}
static int idle_owner_send_hook(uint32_t ep,SfMessage*m,int64_t*out){
 if(!capture_hook_enabled||ep!=0x101||m->h.opcode!=OP_PERSIST_SNAPSHOT)return 0;
 capture_sends++;capture_request=*m;
 CHECK(m->h.size==sizeof(SfIpcHeader)&&m->h.protocol_major==1&&!m->h.protocol_minor&&
       m->h.opcode==OP_PERSIST_SNAPSHOT&&!m->h.flags&&m->h.request_id==fence_capture.request_id);
 CHECK(m->h.payload_bytes==sizeof(*m)-sizeof(SfIpcHeader)&&!m->h.cap_count&&
       m->h.deadline_ns==fence_capture.deadline_ns&&!m->status&&!m->handle&&!m->offset&&!m->length&&!m->mode);
 for(unsigned i=0;i<sizeof(m->data);i++)CHECK(!m->data[i]);
 if(reenter_capture){reenter_capture=0;reenter_result=sf_persist_fence_capture(fence_capture.token);}
 *out=capture_send_result;return 1;
}
static int idle_owner_recv_hook(uint32_t ep,SfMessage*m,uint64_t deadline,int64_t*out){
 if(!capture_hook_enabled||ep!=0x102||m->h.opcode!=OP_PERSIST_SNAPSHOT)return 0;
 capture_receives++;CHECK(deadline==fence_capture.deadline_ns);
 if(capture_recv_result){*out=capture_recv_result;return 1;}
 capture_fill_reply(m);*out=capture_from?capture_from:12;
 if(capture_reply_variant==1){m->h.flags=13;*out=13;}
 if(capture_reply_variant==2){m->h.flags=12;*out=(1ll<<32)+12;}
 return 1;
}

static uint64_t capture_begin(void){
 capture_hook_enabled=0;reset();memset(&fence_capture,0,sizeof(fence_capture));
 memset(policy_keys,0,sizeof(policy_keys));
 for(unsigned i=0;i<32;i++)policy_keys[0].public_key[i]=(uint8_t)(i+9);
 policy_keys[0].usage=SF_TRUST_MODULE;
 uint64_t token=begin();snapshot_state();
 capture_reply_status=capture_reply_variant=capture_send_result=capture_from=0;capture_recv_result=0;
 capture_mutate_after=capture_mutate_kind=0;
 reenter_capture=reenter_result=0;capture_sends=capture_receives=0;
 memset(&capture_request,0,sizeof(capture_request));capture_hook_enabled=1;return token;
}
static SfPersistCaptureStatus capture_report(uint64_t token){
 SfPersistCaptureStatus s;memset(&s,0xa5,sizeof(s));CHECK(!sf_persist_fence_capture_status(token,&s));CHECK(s.token==token);return s;
}
static void output_zero(const SfPersistSnapshot*s){SfPersistSnapshot z;memset(&z,0,sizeof(z));CHECK(!memcmp(s,&z,sizeof(z)));}
static void capture_expect_locked(uint64_t token){
 SfPersistStopStatus stop_before,stop_after;CHECK(!sf_persist_fence_stop_status(token,&stop_before));
 unsigned sends0=sends,receives0=receives,cs=capture_sends,cr=capture_receives;
 int qexpected=uncertain_commit?SF_COMMIT_UNCERTAIN:SF_DRAINING;
 CHECK(sf_persist_fence_capture(token)==SF_DRAINING);
 CHECK(sf_persist_fence_cancel(token)==SF_DRAINING);
 CHECK(sf_persist_fence_stop(token)==SF_DRAINING);
 uint64_t notes=UINT64_MAX;CHECK(sf_persist_notes_size(&notes)==qexpected&&notes==UINT64_MAX);
 CHECK(sf_persist_quiescent()==qexpected);
 CHECK(sends==sends0&&receives==receives0&&capture_sends==cs&&capture_receives==cr);
 CHECK(!sf_persist_fence_stop_status(token,&stop_after)&&!memcmp(&stop_before,&stop_after,sizeof(stop_before)));
}

static void capture_success_and_stop(void){
 uint64_t token=capture_begin();SfPersistCaptureStatus old;unsigned old_sends=sends,old_receives=receives,old_revokes=revokes;
 CHECK(!sf_persist_fence_capture(token));SfPersistCaptureStatus s=capture_report(token);
 CHECK(s.phase==SF_PERSIST_CAPTURE_CAPTURED&&s.authenticated&&!s.error&&s.pid==12&&s.request_id&&s.deadline_ns);
 CHECK(!s.send_result&&s.receive_result==12&&!s.peer_status&&s.snapshot.size==sizeof(SfPersistSnapshot));
 CHECK(s.snapshot.version==1&&s.snapshot.generation==9&&s.snapshot.notes_bytes==8&&s.snapshot.volume[0]==0x30);
 CHECK(capture_sends==1&&capture_receives==1&&sends==old_sends&&receives==old_receives&&revokes==old_revokes);
 SfPersistCaptureStatus again=capture_report(token);CHECK(!memcmp(&s,&again,sizeof(s)));
 CHECK(sf_persist_fence_capture_status(token+1,&old)==SF_STALE_HANDLE);
 SfPersistCaptureStatus untouched;memset(&untouched,0xa5,sizeof(untouched));SfPersistCaptureStatus before=untouched;
 CHECK(sf_persist_fence_capture_status(token+1,&untouched)==SF_STALE_HANDLE&&!memcmp(&before,&untouched,sizeof(before)));
 snapshot_state();CHECK(sf_persist_fence_capture(token)==SF_DRAINING&&capture_sends==1&&capture_receives==1);
 SfPersistStopStatus stop_before=report(token);CHECK(stop_before.phase==SF_PERSIST_TERMINAL_ACTIVE&&!stop_before.request_id);
 CHECK(!sf_persist_fence_stop(token));SfPersistStopStatus stop_after=report(token);
 CHECK(stop_after.phase==SF_PERSIST_TERMINAL_STOPPED_CLOSED&&stop_after.retired&&revokes==old_revokes+2);
 SfPersistCaptureStatus after=capture_report(token);CHECK(!memcmp(&s,&after,sizeof(s)));
 CHECK(sends==old_sends+1&&receives==old_receives+1&&capture_sends==1&&capture_receives==1);
 unchanged();cases++;
}

static void refusal_and_prephase(void){
 uint64_t token=capture_begin();capture_reply_status=SF_BUSY;
 CHECK(sf_persist_fence_capture(token)==SF_BUSY);SfPersistCaptureStatus s=capture_report(token);
 CHECK(s.phase==SF_PERSIST_CAPTURE_REFUSED&&s.authenticated&&s.error==SF_BUSY&&s.peer_status==SF_BUSY);
 output_zero(&s.snapshot);CHECK(capture_sends==1&&capture_receives==1);
 CHECK(!sf_persist_fence_cancel(token)&&!sf_persist_fenced());cases++;
 token=capture_begin();CHECK(!sf_persist_fence_stop(token));
 unsigned cs=capture_sends,cr=capture_receives;
 CHECK(sf_persist_fence_capture(token)==SF_DRAINING&&capture_sends==cs&&capture_receives==cr);cases++;
}

static void malformed_and_transport(void){
 for(int variant0=1;variant0<=25;variant0++){
  uint64_t token=capture_begin();capture_reply_variant=variant0;
  int expected=variant0==1||variant0==2?SF_BAD_FORMAT:SF_BAD_FORMAT;
  CHECK(sf_persist_fence_capture(token)==expected);
  SfPersistCaptureStatus s=capture_report(token);CHECK(s.phase==SF_PERSIST_CAPTURE_FAILED&&s.error==SF_BAD_FORMAT);
  CHECK(!s.snapshot.size&&!s.snapshot.version&&s.authenticated==(variant0>=20&&variant0<=24));
  CHECK(capture_sends==1&&capture_receives==1&&closes==(variant0==9?1u:0u));
  capture_expect_locked(token);CHECK(!uncertain_commit&&!trust_error);cases++;
 }
 const int64_t receive_errors[]={SF_TIMEOUT,SF_WOULD_BLOCK,SF_DEVICE_GONE,SF_DENIED,1,(1ll<<32)+12};
 for(unsigned i=0;i<sizeof(receive_errors)/sizeof(receive_errors[0]);i++){
  uint64_t token=capture_begin();capture_recv_result=receive_errors[i];
  int expected=receive_errors[i]<0?(int)receive_errors[i]:SF_BAD_FORMAT;
  CHECK(sf_persist_fence_capture(token)==expected);
  SfPersistCaptureStatus s=capture_report(token);CHECK(s.phase==SF_PERSIST_CAPTURE_FAILED&&!s.authenticated);
  capture_expect_locked(token);cases++;
 }
 uint64_t token=capture_begin();capture_send_result=SF_TIMEOUT;
 CHECK(sf_persist_fence_capture(token)==SF_TIMEOUT);CHECK(capture_report(token).phase==SF_PERSIST_CAPTURE_FAILED);
 CHECK(capture_sends==1&&!capture_receives);capture_expect_locked(token);cases++;
 token=capture_begin();capture_reply_status=SF_DEVICE_GONE;
 CHECK(sf_persist_fence_capture(token)==SF_DEVICE_GONE);
 CHECK(capture_report(token).phase==SF_PERSIST_CAPTURE_FAILED&&capture_report(token).authenticated);
 capture_expect_locked(token);cases++;
}

static void preflight_reentrant_and_context(void){
 uint64_t token=capture_begin();CHECK(sf_persist_fence_capture(0)==SF_STALE_HANDLE&&sf_persist_fence_capture(token+1)==SF_STALE_HANDLE);
 CHECK(!capture_sends&&!capture_receives);CHECK(!sf_persist_fence_cancel(token));cases++;
 token=capture_begin();reenter_capture=1;CHECK(!sf_persist_fence_capture(token));
 CHECK(reenter_result==SF_DRAINING&&capture_sends==1&&capture_receives==1);CHECK(!sf_persist_fence_cancel(token));cases++;
 token=capture_begin();requests=UINT64_MAX;CHECK(sf_persist_fence_capture(token)==SF_NO_SPACE&&!capture_sends&&!capture_receives);
 CHECK(!sf_persist_fence_cancel(token));cases++;
 token=capture_begin();clock_error=SF_DENIED;CHECK(sf_persist_fence_capture(token)==SF_BAD_FORMAT&&!capture_sends&&!capture_receives);
 clock_error=0;CHECK(!sf_persist_fence_cancel(token));cases++;
 for(unsigned kind=1;kind<24+RES_COUNT;kind++){
  token=capture_begin();mutate(kind);unsigned before=capture_sends;
  CHECK(sf_persist_fence_capture(token)<0&&capture_sends==before&&capture_receives==0);
  CHECK(sf_persist_fence_cancel(token)<0);cases++;
 }
 for(unsigned kind=1;kind<=3;kind++){
  token=capture_begin();capture_reply_variant=0;
  /* Mutate after constructing a valid reply, before the owner rechecks context. */
  capture_mutate_after=1;capture_mutate_kind=kind==1?5:kind==2?20:21;
  CHECK(sf_persist_fence_capture(token)<0);
  SfPersistCaptureStatus s=capture_report(token);
  CHECK(s.phase==SF_PERSIST_CAPTURE_FAILED&&!s.snapshot.size&&s.authenticated);
  capture_expect_locked(token);cases++;
 }
}

int main(void){
 capture_success_and_stop();refusal_and_prephase();malformed_and_transport();preflight_reentrant_and_context();
 printf("PERSIST SNAPSHOT OWNER PASS %u checks; E1 actual Root owner with mocked kernel/transport; snapshot callback bytes synthetic\n",checks);
 return 0;
}
