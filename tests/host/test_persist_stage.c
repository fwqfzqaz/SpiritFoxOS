/* E1: exercise the real stage wire codec with capability callbacks only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../services/persist_stage.h"
#include "../../services/persist_stage.c"

static unsigned checks,cases;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"PERSIST STAGE FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)

typedef struct {
 int64_t derived[4],close_result;
 uint64_t derive_parent[4],closed[8];
 unsigned derive_rights[4],derives,closes;
} Harness;

static int64_t cap_derive(void*context,uint64_t parent,unsigned rights){
 Harness*h=context;unsigned i=h->derives++;
 if(i<4){h->derive_parent[i]=parent;h->derive_rights[i]=rights;return h->derived[i];}
 return SF_NO_SPACE;
}
static int64_t cap_close(void*context,uint64_t handle){
 Harness*h=context;unsigned i=h->closes++;
 if(i<8)h->closed[i]=handle;
 return h->close_result;
}
static SfPersistStageCaps cap_ops(Harness*h){return(SfPersistStageCaps){h,cap_derive,cap_close};}

static void fill_snapshot(SfPersistSnapshot*s){
 memset(s,0,sizeof(*s));s->size=sizeof(*s);s->version=1;s->generation=9;s->notes_bytes=13;
 for(unsigned i=0;i<sizeof(s->volume);i++)s->volume[i]=(uint8_t)(i+1);
 for(unsigned i=0;i<sizeof(s->files_sha256);i++)s->files_sha256[i]=(uint8_t)(0x20+i);
 for(unsigned i=0;i<sizeof(s->trust_sha256);i++)s->trust_sha256[i]=(uint8_t)(0x80+i);
}
static SfPersistStageBinding valid_binding(void){
 SfPersistStageBinding b;memset(&b,0,sizeof(b));fill_snapshot(&b.snapshot);
 b.token=17;b.storage_instance=29;b.capacity=(uint64_t)SF_TXN_SECTORS*512u;
 b.driver_pid=31;b.fs_pid=37;b.writable=1;b.version=1;return b;
}
static SfMessage request(const SfPersistStageBinding*b,unsigned mode,uint64_t nonce,
                        uint64_t incoming,uint64_t now){
 SfMessage m;memset(&m,0,sizeof(m));
 m.h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_PERSIST_CANDIDATE_CAPS,1,nonce,
                   sizeof(m)-sizeof(SfIpcHeader),1,now+SF_PERSIST_STAGE_NS};
 m.handle=incoming;m.mode=mode;m.length=sizeof(*b);memcpy(m.data,b,sizeof(*b));return m;
}
static int dispatch(SfPersistStage*s,int64_t from,const SfMessage*m,uint64_t now,
                    int available,uint32_t self,const SfPersistSnapshot*expected,Harness*h){
 SfPersistStageCaps c=cap_ops(h);
 return persist_stage_dispatch(s,from,m,now,available,self,expected,&c);
}
static void fresh_failure(const SfMessage*m,int64_t from,uint64_t now,int available,
                          const SfPersistSnapshot*expected,uint32_t self,int expected_error){
 SfPersistStage s;memset(&s,0,sizeof(s));Harness h;memset(&h,0,sizeof(h));
 int r=dispatch(&s,from,m,now,available,self,expected,&h);
 CHECK(r==expected_error&&h.derives==0);
 if(m->h.cap_count==1&&m->handle)CHECK(h.closes==1&&h.closed[0]==m->handle);else CHECK(!h.closes);
 if(r==SF_INVALID||r==SF_DENIED||r==SF_TIMEOUT){CHECK(s.phase==SF_PERSIST_STAGE_NONE&&s.last_nonce==0);}
 if(r==SF_STALE_HANDLE){CHECK(s.phase==SF_PERSIST_STAGE_FAILED&&s.last_nonce==m->h.request_id);}
 cases++;
}

static void valid_two_step_transfer(void){
 const uint64_t now=10000000000ull;SfPersistSnapshot expected;fill_snapshot(&expected);
 SfPersistStageBinding b=valid_binding();SfPersistStage s;memset(&s,0,sizeof(s));
 Harness h;memset(&h,0,sizeof(h));h.derived[0]=501;h.derived[1]=502;
 SfMessage send_req=request(&b,SF_PERSIST_STAGE_SEND,10,101,now);
 CHECK(!dispatch(&s,1,&send_req,now,1,41,&expected,&h));
 CHECK(s.phase==SF_PERSIST_STAGE_SEND_HELD&&!s.fatal&&s.last_nonce==10&&s.send==501&&!s.reply);
 CHECK(!memcmp(&s.binding,&b,sizeof(b))&&h.derives==1&&h.closes==1&&h.closed[0]==101);
 CHECK(h.derive_parent[0]==101&&h.derive_rights[0]==RIGHT_SEND);

 SfMessage replay=send_req;replay.handle=102;
 CHECK(dispatch(&s,1,&replay,now,1,41,&expected,&h)==SF_STALE_HANDLE);
 CHECK(s.phase==SF_PERSIST_STAGE_SEND_HELD&&s.last_nonce==10&&h.derives==1&&h.closes==2&&h.closed[1]==102);

 SfMessage reply_req=request(&b,SF_PERSIST_STAGE_REPLY,11,103,now);
 CHECK(!dispatch(&s,1,&reply_req,now,1,41,&expected,&h));
 CHECK(s.phase==SF_PERSIST_STAGE_READY&&!s.fatal&&s.last_nonce==11&&s.send==501&&s.reply==502);
 CHECK(h.derives==2&&h.closes==3&&h.closed[2]==103&&h.derive_parent[1]==103&&
       h.derive_rights[1]==RIGHT_RECV);
 SfMessage retry=request(&b,SF_PERSIST_STAGE_REPLY,12,104,now);
 CHECK(dispatch(&s,1,&retry,now,1,41,&expected,&h)==SF_DRAINING);
 CHECK(s.phase==SF_PERSIST_STAGE_READY&&s.last_nonce==12&&h.derives==2&&h.closes==4);
 cases++;
}

static void malformed_wire_and_ownership(void){
 const uint64_t now=20000000000ull;SfPersistStageBinding b=valid_binding();
 SfPersistSnapshot expected=b.snapshot;SfMessage base=request(&b,SF_PERSIST_STAGE_SEND,1,201,now);
 SfMessage m=base;m.h.size++;
 fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.h.protocol_major=2;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.h.protocol_minor=1;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.h.opcode++;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.h.flags=2;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.h.request_id=0;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.h.cap_count=0;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.h.cap_count=2;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.h.payload_bytes--;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.h.deadline_ns=0;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.status=1;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.offset=1;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.length--;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.mode=0;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.mode=99;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.handle=0;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.handle=2147483648ull;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 m=base;m.h.deadline_ns=now;fresh_failure(&m,1,now,1,&expected,41,SF_TIMEOUT);
 m=base;m.h.deadline_ns=now+SF_PERSIST_STAGE_NS+1;
 fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 fresh_failure(&base,2,now,1,&expected,41,SF_DENIED);
 for(unsigned i=sizeof(b);i<sizeof(base.data);i++){
  SfMessage m=base;m.data[i]=1;fresh_failure(&m,1,now,1,&expected,41,SF_INVALID);
 }
 SfMessage no_cap=base;no_cap.h.cap_count=0;
 fresh_failure(&no_cap,1,now,1,&expected,41,SF_INVALID);
 SfMessage two_caps=base;two_caps.h.cap_count=2;
 fresh_failure(&two_caps,1,now,1,&expected,41,SF_INVALID);
}

static void binding_validation_and_drift(void){
 const uint64_t now=30000000000ull;SfPersistStageBinding b=valid_binding();
 SfPersistSnapshot expected=b.snapshot;SfMessage base=request(&b,SF_PERSIST_STAGE_SEND,1,301,now);
 /* Each valid-looking wire payload differs from the authenticated live binding. */
 for(unsigned field=0;field<15;field++){
  SfPersistStageBinding bad=b;
  switch(field){
   case 0:bad.version++;break;case 1:bad.token=0;break;case 2:bad.storage_instance=0;break;
   case 3:bad.capacity++;break;case 4:bad.capacity=511;break;case 5:bad.writable=0;break;
   case 6:bad.driver_pid=1;break;case 7:bad.fs_pid=1;break;
   case 8:bad.driver_pid=bad.fs_pid;break;case 9:bad.fs_pid=41;break;
   case 10:bad.snapshot.notes_bytes=SF_TXN_FILE_BYTES+1;break;
   case 11:bad.snapshot.volume[0]^=1;break;case 12:bad.snapshot.size++;break;
   case 13:bad.snapshot.version++;break;case 14:bad.snapshot.generation=0;break;
  }
  SfMessage m=request(&bad,SF_PERSIST_STAGE_SEND,1,302+field,now);
  fresh_failure(&m,1,now,1,&expected,41,SF_STALE_HANDLE);
 }
 for(unsigned field=0;field<4;field++){
  uint32_t self=41;SfPersistStageBinding good=b;
  if(field==0)self=1;if(field==1)self=b.driver_pid;
  if(field==2)good.driver_pid=self;if(field==3)good.fs_pid=self;
  SfMessage m=request(&good,SF_PERSIST_STAGE_SEND,1,320+field,now);
  fresh_failure(&m,1,now,1,&expected,self,SF_STALE_HANDLE);
 }
 {
  SfMessage m=request(&b,SF_PERSIST_STAGE_SEND,1,329,now);
  fresh_failure(&m,1,now,1,&expected,2147483648u,SF_STALE_HANDLE);
 }
 for(unsigned field=0;field<2;field++){
  SfPersistStageBinding bad=b;if(field==0)bad.driver_pid=2147483648u;else bad.fs_pid=2147483648u;
  SfMessage m=request(&bad,SF_PERSIST_STAGE_SEND,1,330+field,now);
  fresh_failure(&m,1,now,1,&expected,41,SF_STALE_HANDLE);
 }
 {
  SfPersistStageBinding bad=b;bad.capacity=(uint64_t)SF_TXN_SECTORS*512u-512u;
  SfMessage m=request(&bad,SF_PERSIST_STAGE_SEND,1,332,now);
  fresh_failure(&m,1,now,1,&expected,41,SF_STALE_HANDLE);
 }
 SfMessage m=base;SfPersistSnapshot changed=expected;changed.generation++;
 fresh_failure(&m,1,now,1,&changed,41,SF_STALE_HANDLE);
}

static void ordering_availability_and_clock_edges(void){
 const uint64_t now=40000000000ull;SfPersistStageBinding b=valid_binding();
 SfPersistSnapshot expected=b.snapshot;Harness h;SfPersistStage s;
 memset(&s,0,sizeof(s));memset(&h,0,sizeof(h));h.derived[0]=601;h.derived[1]=602;
 SfMessage first=request(&b,SF_PERSIST_STAGE_SEND,20,401,now);
 CHECK(!dispatch(&s,1,&first,now,1,41,&expected,&h));
 SfMessage duplicate=first;duplicate.handle=402;
 CHECK(dispatch(&s,1,&duplicate,now,1,41,&expected,&h)==SF_STALE_HANDLE);
 CHECK(s.phase==SF_PERSIST_STAGE_SEND_HELD&&s.last_nonce==20&&h.derives==1);
 SfMessage wrong_order=request(&b,SF_PERSIST_STAGE_SEND,21,403,now);
 CHECK(dispatch(&s,1,&wrong_order,now,1,41,&expected,&h)==SF_DRAINING);
 CHECK(s.phase==SF_PERSIST_STAGE_FAILED&&s.last_nonce==21&&h.derives==1);
 SfMessage no_retry=request(&b,SF_PERSIST_STAGE_REPLY,22,404,now);
 CHECK(dispatch(&s,1,&no_retry,now,1,41,&expected,&h)==SF_DRAINING&&h.derives==1);

 memset(&s,0,sizeof(s));memset(&h,0,sizeof(h));SfMessage unavailable=request(&b,SF_PERSIST_STAGE_SEND,30,405,now);
 CHECK(dispatch(&s,1,&unavailable,now,0,41,&expected,&h)==SF_BUSY);
 CHECK(s.phase==SF_PERSIST_STAGE_FAILED&&s.last_nonce==30&&h.closes==1&&!h.derives);
 unavailable.h.request_id=31;unavailable.handle=406;
 CHECK(dispatch(&s,1,&unavailable,now,1,41,&expected,&h)==SF_DRAINING&&h.closes==2&&!h.derives);
 memset(&s,0,sizeof(s));memset(&h,0,sizeof(h));
 SfMessage missing_expected=request(&b,SF_PERSIST_STAGE_SEND,32,410,now);
 CHECK(dispatch(&s,1,&missing_expected,now,1,41,0,&h)==SF_BUSY);
 CHECK(s.phase==SF_PERSIST_STAGE_FAILED&&s.last_nonce==32&&h.closes==1&&!h.derives);

 memset(&s,0,sizeof(s));memset(&h,0,sizeof(h));h.derived[0]=607;
 SfMessage saturated=request(&b,SF_PERSIST_STAGE_SEND,40,407,UINT64_MAX-1);
 saturated.h.deadline_ns=UINT64_MAX;
 CHECK(!dispatch(&s,1,&saturated,UINT64_MAX-1,1,41,&expected,&h));
 CHECK(s.phase==SF_PERSIST_STAGE_SEND_HELD&&s.last_nonce==40&&h.derives==1&&h.closes==1);
 memset(&s,0,sizeof(s));memset(&h,0,sizeof(h));h.derived[0]=608;
 SfMessage max_nonce=request(&b,SF_PERSIST_STAGE_SEND,UINT64_MAX,408,now);
 CHECK(!dispatch(&s,1,&max_nonce,now,1,41,&expected,&h)&&s.last_nonce==UINT64_MAX);
 SfMessage after_max=request(&b,SF_PERSIST_STAGE_REPLY,1,409,now);
 CHECK(dispatch(&s,1,&after_max,now,1,41,&expected,&h)==SF_STALE_HANDLE&&
       s.phase==SF_PERSIST_STAGE_SEND_HELD&&h.derives==1);
 cases++;
}

static void derive_close_failures(void){
 const uint64_t now=50000000000ull;SfPersistStageBinding b=valid_binding();
 SfPersistSnapshot expected=b.snapshot;int64_t results[]={SF_DENIED,0,SF_VERSION_REJECTED-1,2147483648ll};
 int statuses[]={SF_DENIED,SF_BAD_FORMAT,SF_BAD_FORMAT,SF_BAD_FORMAT};
 for(unsigned i=0;i<sizeof(results)/sizeof(*results);i++){
  SfPersistStage s;memset(&s,0,sizeof(s));Harness h;memset(&h,0,sizeof(h));h.derived[0]=results[i];
  SfMessage m=request(&b,SF_PERSIST_STAGE_SEND,50+i,501+i,now);
  CHECK(dispatch(&s,1,&m,now,1,41,&expected,&h)==statuses[i]);
  CHECK(s.phase==SF_PERSIST_STAGE_FAILED&&s.last_nonce==50+i&&h.derives==1&&
        h.derive_rights[0]==RIGHT_SEND&&h.closes==1&&h.closed[0]==501+i);
  CHECK((s.fatal!=0)==(i==3));
  SfMessage retry=request(&b,SF_PERSIST_STAGE_SEND,60+i,511+i,now);
  CHECK(dispatch(&s,1,&retry,now,1,41,&expected,&h)==SF_DRAINING&&h.derives==1);
 }
 /* The limiting callback must return a distinct handle, never the receipt. */
 {
  SfPersistStage s;memset(&s,0,sizeof(s));Harness h;memset(&h,0,sizeof(h));h.derived[0]=601;
  SfMessage m=request(&b,SF_PERSIST_STAGE_SEND,70,601,now);
  CHECK(dispatch(&s,1,&m,now,1,41,&expected,&h)==SF_BAD_FORMAT);
  CHECK(s.phase==SF_PERSIST_STAGE_FAILED&&s.fatal&&h.derives==1&&h.closes==1&&h.closed[0]==601);
 }
 {
  SfPersistStage s;memset(&s,0,sizeof(s));Harness h;memset(&h,0,sizeof(h));h.derived[0]=SF_NO_SPACE;
  SfMessage m=request(&b,SF_PERSIST_STAGE_SEND,74,714,now);
  CHECK(dispatch(&s,1,&m,now,1,41,&expected,&h)==SF_NO_SPACE);
  CHECK(s.phase==SF_PERSIST_STAGE_FAILED&&!s.fatal&&h.closes==1&&h.closed[0]==714);
 }
 /* STALE means the receipt is already absent; other close errors are fatal. */
 {
  SfPersistStage s;memset(&s,0,sizeof(s));Harness h;memset(&h,0,sizeof(h));h.derived[0]=701;h.close_result=SF_STALE_HANDLE;
  SfMessage m=request(&b,SF_PERSIST_STAGE_SEND,71,702,now);
  CHECK(!dispatch(&s,1,&m,now,1,41,&expected,&h)&&s.phase==SF_PERSIST_STAGE_SEND_HELD&&!s.fatal);
 }
 {
  SfPersistStage s;memset(&s,0,sizeof(s));Harness h;memset(&h,0,sizeof(h));h.derived[0]=711;h.close_result=SF_VERSION_REJECTED-1;
  SfMessage m=request(&b,SF_PERSIST_STAGE_SEND,72,712,now);
  CHECK(dispatch(&s,1,&m,now,1,41,&expected,&h)==SF_BAD_FORMAT);
  CHECK(s.phase==SF_PERSIST_STAGE_FAILED&&s.fatal&&s.send==711&&h.closes==1);
 }
 {
  SfPersistStage s;memset(&s,0,sizeof(s));Harness h;memset(&h,0,sizeof(h));h.close_result=SF_DENIED;
  SfMessage m=request(&b,SF_PERSIST_STAGE_SEND,73,713,now);m.h.flags++;
  CHECK(dispatch(&s,1,&m,now,1,41,&expected,&h)==SF_BAD_FORMAT);
  CHECK(s.phase==SF_PERSIST_STAGE_FAILED&&s.fatal&&!h.derives&&h.closes==1);
 }
 cases++;
}

static void reply_is_canonical_and_bounded(void){
 const uint64_t now=60000000000ull;SfPersistStageBinding b=valid_binding();
 SfMessage m;memset(&m,0xa5,sizeof(m));m.h.request_id=81;m.h.deadline_ns=now+100;
 persist_stage_reply(&m,0,now,SF_PERSIST_STAGE_SEND,&b);
 CHECK(m.h.size==sizeof(SfIpcHeader)&&m.h.protocol_major==1&&!m.h.protocol_minor&&
       m.h.opcode==OP_PERSIST_CANDIDATE_CAPS&&!m.h.flags&&m.h.request_id==81&&
       m.h.payload_bytes==sizeof(m)-sizeof(SfIpcHeader)&&!m.h.cap_count&&
       m.h.deadline_ns==now+100&&!m.handle&&!m.offset&&m.mode==SF_PERSIST_STAGE_SEND&&
       !m.status&&m.length==sizeof(b)&&!memcmp(m.data,&b,sizeof(b)));
 for(unsigned i=sizeof(b);i<sizeof(m.data);i++)CHECK(!m.data[i]);
 memset(&m,0xa5,sizeof(m));m.h.request_id=82;m.h.deadline_ns=now;
 persist_stage_reply(&m,SF_BUSY,now,SF_PERSIST_STAGE_REPLY,&b);
 CHECK(m.h.request_id==82&&m.h.deadline_ns==now+SF_PERSIST_STAGE_NS&&
       m.status==SF_BUSY&&!m.mode&&!m.handle&&!m.offset&&!m.length);
 for(unsigned i=0;i<sizeof(m.data);i++)CHECK(!m.data[i]);
 memset(&m,0,sizeof(m));m.h.request_id=83;m.h.deadline_ns=now+SF_PERSIST_STAGE_NS+1;
 persist_stage_reply(&m,0,now,SF_PERSIST_STAGE_REPLY,&b);
 CHECK(m.h.deadline_ns==now+SF_PERSIST_STAGE_NS&&m.mode==SF_PERSIST_STAGE_REPLY);
 memset(&m,0,sizeof(m));m.h.request_id=84;m.h.deadline_ns=now+100;
 persist_stage_reply(&m,0,now,0,&b);
 CHECK(m.status==SF_INVALID&&!m.mode&&!m.length);
 memset(&m,0,sizeof(m));m.h.request_id=85;m.h.deadline_ns=now+100;
 persist_stage_reply(&m,0,now,SF_PERSIST_STAGE_SEND,0);
 CHECK(m.status==SF_INVALID&&!m.mode&&!m.length);
 cases++;
}

static void deterministic_vectors_1000(void){
 const uint64_t now=70000000000ull;SfPersistSnapshot expected;fill_snapshot(&expected);
 for(uint64_t i=1;i<=1000;i++){
  SfPersistStageBinding b=valid_binding();b.token=i+10;b.storage_instance=i+100;
  b.snapshot.generation=i;b.snapshot.notes_bytes=i&1?SF_TXN_FILE_BYTES:13;
  SfPersistStage s;memset(&s,0,sizeof(s));Harness h;memset(&h,0,sizeof(h));
  h.derived[0]=(int64_t)(10000+i);SfMessage m=request(&b,SF_PERSIST_STAGE_SEND,i,(uint64_t)(20000+i),now);
  expected=b.snapshot;
  CHECK(!dispatch(&s,1,&m,now,1,41,&expected,&h));
  CHECK(s.phase==SF_PERSIST_STAGE_SEND_HELD&&s.last_nonce==i&&s.send==10000+i&&
        !s.reply&&!s.fatal&&h.derives==1&&h.closes==1&&
        h.derive_parent[0]==20000+i&&h.derive_rights[0]==RIGHT_SEND&&
        h.closed[0]==20000+i&&!memcmp(&s.binding,&b,sizeof(b)));
 }
 cases++;
}

int main(void){
 valid_two_step_transfer();malformed_wire_and_ownership();binding_validation_and_drift();
 ordering_availability_and_clock_edges();derive_close_failures();reply_is_canonical_and_bounded();
 deterministic_vectors_1000();
 printf("PERSIST STAGE CODEC PASS: %u assertions/%u cases\n",checks,cases);return 0;
}
