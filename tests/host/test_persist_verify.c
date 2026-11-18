/* E1: production opcode 109 parser/read bridge with an isolated transport and
 * volume callback. It tests protocol behavior, not transaction authentication
 * or guest IPC; the actual transaction-volume verifier has a separate fixture. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LIBSF_H
#include "sf.h"
#include "../../services/persist_verify.c"

static unsigned checks,cases;
#define VCHECK(c) do{checks++;if(!(c)){fprintf(stderr,"persist verify failure line %d: %s\n",__LINE__,#c);exit(1);}}while(0)

enum { RX_OK,RX_SIZE,RX_MAJOR,RX_MINOR,RX_OPCODE,RX_FLAGS,RX_NONCE,RX_CAPS,
 RX_PAYLOAD,RX_DEADLINE,RX_MODE,RX_LBA,RX_SEQUENCE,RX_LENGTH,RX_TAIL,
 RX_POSITIVE,RX_PEER_BUSY,RX_FROM_WRONG,RX_CAPS_WIDE };
typedef struct {
 SfMessage request;
 unsigned sends,receives,closes,volume_calls,reads;
 int send_error;int64_t receive_error;int now_error,now_error_at;
 uint64_t now_value;unsigned now_calls,expire_after_reply;
 unsigned rx_mutation,close_handle,want_reads;
 int volume_error,volume_mutate;
 uint64_t first_lba;
 SfPersistSnapshot expected;
} Fixture;
static Fixture*active_fixture;

static void expected_snapshot(SfPersistSnapshot*s){
 memset(s,0,sizeof(*s));s->size=sizeof(*s);s->version=1;s->generation=77;s->notes_bytes=19;
 for(unsigned i=0;i<sizeof(s->volume);i++)s->volume[i]=(uint8_t)(i+3);
 for(unsigned i=0;i<sizeof(s->files_sha256);i++)s->files_sha256[i]=(uint8_t)(0x30+i);
 for(unsigned i=0;i<sizeof(s->trust_sha256);i++)s->trust_sha256[i]=(uint8_t)(0xa0+i);
}
static void request_make(SfMessage*m,const SfPersistSnapshot*s,uint64_t nonce,uint64_t now,uint64_t deadline){
 memset(m,0,sizeof(*m));
 m->h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_PERSIST_CANDIDATE_VERIFY,1,nonce,
                    sizeof(*m)-sizeof(SfIpcHeader),0,deadline};
 m->length=sizeof(*s);memcpy(m->data,s,sizeof(*s));
 (void)now;
}
static void sector_fill(uint64_t lba,uint8_t out[512]){
 for(unsigned i=0;i<512;i++)out[i]=(uint8_t)(lba*17u+i*29u+(lba>>3));
}
static int transport_now(void*context,uint64_t*out){
 Fixture*f=context;f->now_calls++;if(f->now_error&&(!f->now_error_at||f->now_calls==(unsigned)f->now_error_at))return f->now_error;*out=f->now_value;return 0;
}
static int64_t transport_send(void*context,SfMessage*m){
 Fixture*f=context;f->sends++;f->request=*m;
 VCHECK(m->h.size==sizeof(SfIpcHeader)&&m->h.protocol_major==1&&!m->h.protocol_minor&&
        m->h.opcode==OP_PERSIST_CANDIDATE_VERIFY&&m->h.flags==0&&m->h.request_id&&
        m->h.cap_count==0&&m->h.payload_bytes==sizeof(*m)-sizeof(SfIpcHeader)&&
        m->h.deadline_ns>f->now_value&&m->mode==1&&m->offset>=SF_TXN_HEADER&&
        m->offset<SF_TXN_BANK+2*SF_TXN_BANK_SECTORS&&m->length==512&&m->handle>0);
 for(unsigned i=0;i<sizeof(m->data);i++)VCHECK(!m->data[i]);
 return f->send_error;
}
static int64_t transport_receive(void*context,SfMessage*m,uint64_t deadline){
 Fixture*f=context;f->receives++;
 if(f->receive_error)return f->receive_error;
 SfMessage sent=*m;memset(m,0,sizeof(*m));m->h=sent.h;m->h.flags=1;
 m->mode=1;m->offset=sent.offset;m->length=512;m->handle=sent.handle;
 for(unsigned i=0;i<512;i++)m->data[i]=(uint8_t)(sent.offset*17u+i*29u+(sent.offset>>3));
 if(deadline!=sent.h.deadline_ns)m->h.deadline_ns=deadline;
 switch(f->rx_mutation){
  case RX_SIZE:m->h.size--;break;case RX_MAJOR:m->h.protocol_major++;break;
  case RX_MINOR:m->h.protocol_minor++;break;case RX_OPCODE:m->h.opcode++;break;
  case RX_FLAGS:m->h.flags=2;break;case RX_NONCE:m->h.request_id++;break;
  case RX_CAPS:m->h.cap_count=1;m->handle=0x777;break;
  case RX_PAYLOAD:m->h.payload_bytes--;break;case RX_DEADLINE:m->h.deadline_ns++;break;
  case RX_MODE:m->mode=0;break;case RX_LBA:m->offset++;break;
  case RX_SEQUENCE:m->handle++;break;case RX_LENGTH:m->length--;break;
  case RX_TAIL:m->data[512]=1;break;case RX_POSITIVE:m->status=1;m->length=0;memset(m->data,0,sizeof(m->data));break;
  case RX_PEER_BUSY:m->status=SF_BUSY;m->length=0;memset(m->data,0,sizeof(m->data));break;
  case RX_CAPS_WIDE:m->h.cap_count=2;break;
  default:break;
 }
 if(f->expire_after_reply)f->now_value=sent.h.deadline_ns;
 return f->rx_mutation==RX_FROM_WRONG?2:1;
}
static void transport_close(void*context,uint64_t handle){Fixture*f=context;f->closes++;f->close_handle=(unsigned)handle;}
static int volume_verify(const SfPersistSnapshot*expected,SfTxnRead read,void*context,SfPersistSnapshot*out){
 Fixture*f=active_fixture;f->volume_calls++;
 unsigned count=f->want_reads?f->want_reads:1;
 for(unsigned i=0;i<count;i++){
  uint8_t sector[512],want[512];uint64_t lba=f->first_lba+i;
  f->reads++;
  int r=read(context,lba,sector);if(r)return r;
  sector_fill(lba,want);if(memcmp(sector,want,sizeof(want)))return SF_BAD_FORMAT;
 }
 if(f->volume_error)return f->volume_error;
 *out=*expected;if(f->volume_mutate)out->generation++;
 return 0;
}

static SfPersistVerifyTransport transport(Fixture*f){
 SfPersistVerifyTransport t={f,transport_send,transport_receive,transport_now,transport_close};return t;
}
static void fixture_init(Fixture*f){
 memset(f,0,sizeof(*f));f->now_value=100;f->first_lba=SF_TXN_HEADER;f->want_reads=1;
 expected_snapshot(&f->expected);
}
static int dispatch(Fixture*f,SfPersistVerify*state,int64_t sender,const SfMessage*request,
                    uint64_t now,int available,SfPersistSnapshot*out){
 SfPersistVerifyTransport t=transport(f);
 active_fixture=f;
 return persist_verify_dispatch(state,sender,request,now,available,volume_verify,&t,out);
}
static void out_sentinel(SfPersistSnapshot*out,SfPersistSnapshot*before){
 memset(out,0xa5,sizeof(*out));*before=*out;
}
static void request_valid(Fixture*f,SfMessage*m,uint64_t nonce,uint64_t now,uint64_t deadline){
 request_make(m,&f->expected,nonce,now,deadline);
}
static void expect_no_output(const SfPersistSnapshot*out,const SfPersistSnapshot*before){
 VCHECK(!memcmp(out,before,sizeof(*out)));
}

static void valid_and_one_shot(void){
 Fixture f;fixture_init(&f);SfPersistVerify state={0};SfMessage request;
 SfPersistSnapshot out,before;out_sentinel(&out,&before);request_valid(&f,&request,7,100,100+SF_PERSIST_VERIFY_NS);
 VCHECK(!dispatch(&f,&state,1,&request,100,1,&out));
 VCHECK(!memcmp(&out,&f.expected,sizeof(out))&&state.last_nonce==7&&state.attempted==1);
 VCHECK(f.volume_calls==1&&f.reads==1&&f.sends==1&&f.receives==1&&f.closes==0);
 request_valid(&f,&request,8,100,100+SF_PERSIST_VERIFY_NS);
 VCHECK(dispatch(&f,&state,1,&request,100,1,&out)==SF_DRAINING);
 VCHECK(f.volume_calls==1&&f.sends==1&&f.receives==1);cases++;
}

static void request_rejections(void){
 enum { BAD_SIZE,BAD_MAJOR,BAD_MINOR,BAD_OPCODE,BAD_FLAGS,BAD_NONCE,BAD_CAPS,
        BAD_PAYLOAD,BAD_DEADLINE,BAD_STATUS,BAD_HANDLE,BAD_OFFSET,BAD_MODE,
        BAD_LENGTH,BAD_TAIL,BAD_COUNT };
 for(unsigned bad=0;bad<BAD_COUNT;bad++){
  Fixture f;fixture_init(&f);SfPersistVerify state={0};SfMessage request;
  request_valid(&f,&request,11,100,100+SF_PERSIST_VERIFY_NS);
  switch(bad){
   case BAD_SIZE:request.h.size--;break;case BAD_MAJOR:request.h.protocol_major++;break;
   case BAD_MINOR:request.h.protocol_minor++;break;case BAD_OPCODE:request.h.opcode++;break;
   case BAD_FLAGS:request.h.flags=0;break;case BAD_NONCE:request.h.request_id=0;break;
   case BAD_CAPS:request.h.cap_count=1;break;case BAD_PAYLOAD:request.h.payload_bytes--;break;
   case BAD_DEADLINE:request.h.deadline_ns=0;break;case BAD_STATUS:request.status=1;break;
   case BAD_HANDLE:request.handle=1;break;case BAD_OFFSET:request.offset=1;break;
   case BAD_MODE:request.mode=1;break;case BAD_LENGTH:request.length--;break;
   default:request.data[sizeof(SfPersistSnapshot)]=1;break;
  }
  SfPersistSnapshot out,before;out_sentinel(&out,&before);
  VCHECK(dispatch(&f,&state,1,&request,100,1,&out)==SF_INVALID);
  expect_no_output(&out,&before);VCHECK(!f.volume_calls&&!f.reads&&!f.sends&&!f.receives&&!state.last_nonce&&!state.attempted);
  /* Invalid shape does not burn its nonce; a corrected request can use it. */
  request_valid(&f,&request,11,100,100+SF_PERSIST_VERIFY_NS);
  VCHECK(!dispatch(&f,&state,1,&request,100,1,&out));cases++;
 }
 Fixture f;fixture_init(&f);SfPersistVerify state={0};SfMessage request;request_valid(&f,&request,1,100,100+SF_PERSIST_VERIFY_NS);
 SfPersistSnapshot out,before;out_sentinel(&out,&before);
 VCHECK(dispatch(&f,&state,2,&request,100,1,&out)==SF_DENIED&&!f.volume_calls&&!f.sends);
 expect_no_output(&out,&before);cases++;
}

static void request_bounds_nonce_and_available(void){
 const uint64_t deadlines[]={100,100+SF_PERSIST_VERIFY_NS+1};
 const int results[]={SF_TIMEOUT,SF_INVALID};
 for(unsigned i=0;i<2;i++){
  Fixture f;fixture_init(&f);SfPersistVerify s={0};SfMessage m;request_valid(&f,&m,20,100,deadlines[i]);
  SfPersistSnapshot out,before;out_sentinel(&out,&before);
  VCHECK(dispatch(&f,&s,1,&m,100,1,&out)==results[i]);expect_no_output(&out,&before);
  VCHECK(!s.last_nonce&&!s.attempted&&!f.volume_calls&&!f.sends);cases++;
 }
 Fixture f;fixture_init(&f);SfPersistVerify s={0};SfMessage m;SfPersistSnapshot out,before;
 request_valid(&f,&m,21,100,100+SF_PERSIST_VERIFY_NS);s.last_nonce=21;out_sentinel(&out,&before);
 VCHECK(dispatch(&f,&s,1,&m,100,1,&out)==SF_STALE_HANDLE&&!f.volume_calls&&!f.sends);
 expect_no_output(&out,&before);cases++;
 fixture_init(&f);memset(&s,0,sizeof(s));request_valid(&f,&m,22,100,100+SF_PERSIST_VERIFY_NS);out_sentinel(&out,&before);
 VCHECK(dispatch(&f,&s,1,&m,100,0,&out)==SF_BUSY&&s.last_nonce==22&&!s.attempted&&!f.volume_calls&&!f.sends);
 expect_no_output(&out,&before);request_valid(&f,&m,23,100,100+SF_PERSIST_VERIFY_NS);
 VCHECK(!dispatch(&f,&s,1,&m,100,1,&out));cases++;
}

static void snapshot_and_read_limits(void){
 for(unsigned field=0;field<4;field++){
  Fixture f;fixture_init(&f);SfPersistVerify s={0};SfMessage m;request_valid(&f,&m,30,100,100+SF_PERSIST_VERIFY_NS);
  SfPersistSnapshot bad;memcpy(&bad,m.data,sizeof(bad));
  if(field==0)bad.size--;else if(field==1)bad.version++;else if(field==2)bad.generation=0;else bad.notes_bytes=SF_TXN_FILE_BYTES+1;
  memcpy(m.data,&bad,sizeof(bad));SfPersistSnapshot out,before;out_sentinel(&out,&before);
  VCHECK(dispatch(&f,&s,1,&m,100,1,&out)==SF_INVALID&&s.attempted&&s.last_nonce==30);
  VCHECK(!f.volume_calls&&!f.sends&&!f.receives);expect_no_output(&out,&before);cases++;
 }
 for(unsigned first=0;first<2;first++){
  Fixture f;fixture_init(&f);SfPersistVerify s={0};SfMessage m;request_valid(&f,&m,31+first,100,100+SF_PERSIST_VERIFY_NS);
  f.first_lba=first?SF_TXN_BANK+2*SF_TXN_BANK_SECTORS:SF_TXN_HEADER-1;
  SfPersistSnapshot out,before;out_sentinel(&out,&before);
  VCHECK(dispatch(&f,&s,1,&m,100,1,&out)==SF_INVALID);
  VCHECK(f.volume_calls==1&&f.reads==1&&!f.sends);expect_no_output(&out,&before);cases++;
 }
 Fixture f;fixture_init(&f);f.want_reads=SF_PERSIST_VERIFY_READS;SfPersistVerify s={0};SfMessage m;
 request_valid(&f,&m,40,100,100+SF_PERSIST_VERIFY_NS);SfPersistSnapshot out,before;out_sentinel(&out,&before);
 VCHECK(!dispatch(&f,&s,1,&m,100,1,&out)&&f.reads==SF_PERSIST_VERIFY_READS&&
        f.sends==SF_PERSIST_VERIFY_READS&&f.receives==SF_PERSIST_VERIFY_READS);cases++;
 fixture_init(&f);f.want_reads=SF_PERSIST_VERIFY_READS+1;memset(&s,0,sizeof(s));
 request_valid(&f,&m,41,100,100+SF_PERSIST_VERIFY_NS);out_sentinel(&out,&before);
 VCHECK(dispatch(&f,&s,1,&m,100,1,&out)==SF_INVALID&&f.sends==SF_PERSIST_VERIFY_READS&&
        f.receives==SF_PERSIST_VERIFY_READS);expect_no_output(&out,&before);cases++;
}

static void reply_variants(void){
 for(unsigned mutation=RX_SIZE;mutation<=RX_CAPS_WIDE;mutation++){
  Fixture f;fixture_init(&f);f.rx_mutation=mutation;SfPersistVerify s={0};SfMessage m;
  request_valid(&f,&m,50+mutation,100,100+SF_PERSIST_VERIFY_NS);SfPersistSnapshot out,before;out_sentinel(&out,&before);
  int r=dispatch(&f,&s,1,&m,100,1,&out);
  VCHECK(r==(mutation==RX_PEER_BUSY?SF_BUSY:SF_BAD_FORMAT));expect_no_output(&out,&before);
  if(mutation==RX_CAPS)VCHECK(f.closes==1&&f.close_handle==0x777);
  else VCHECK(!f.closes);
  cases++;
 }
}

static void error_and_clock_paths(void){
 const int send_results[]={SF_TIMEOUT,SF_VERSION_REJECTED,SF_VERSION_REJECTED-1,1,(1<<30)};
 for(unsigned i=0;i<sizeof(send_results)/sizeof(send_results[0]);i++){
  Fixture f;fixture_init(&f);f.send_error=send_results[i];SfPersistVerify s={0};SfMessage m;
  request_valid(&f,&m,70+i,100,100+SF_PERSIST_VERIFY_NS);SfPersistSnapshot out,before;out_sentinel(&out,&before);
  int expected=i<2?send_results[i]:SF_BAD_FORMAT;
  VCHECK(dispatch(&f,&s,1,&m,100,1,&out)==expected&&f.sends==1&&!f.receives);
  expect_no_output(&out,&before);cases++;
 }
 const int64_t recv_results[]={SF_TIMEOUT,SF_DEVICE_GONE,SF_VERSION_REJECTED-1,1,(1ll<<32)+1};
 for(unsigned i=0;i<sizeof(recv_results)/sizeof(recv_results[0]);i++){
  Fixture f;fixture_init(&f);f.receive_error=recv_results[i];SfPersistVerify s={0};SfMessage m;
  request_valid(&f,&m,80+i,100,100+SF_PERSIST_VERIFY_NS);SfPersistSnapshot out,before;out_sentinel(&out,&before);
  int expected=i<2?(int)recv_results[i]:SF_BAD_FORMAT;
  VCHECK(dispatch(&f,&s,1,&m,100,1,&out)==expected&&f.sends==1&&f.receives==1);
  expect_no_output(&out,&before);cases++;
 }
 for(unsigned where=1;where<=3;where++){
  Fixture f;fixture_init(&f);f.now_error=SF_TIMEOUT;f.now_error_at=(int)where;SfPersistVerify s={0};SfMessage m;
  request_valid(&f,&m,90+where,100,100+SF_PERSIST_VERIFY_NS);
  SfPersistSnapshot out,before;out_sentinel(&out,&before);
  VCHECK(dispatch(&f,&s,1,&m,100,1,&out)==SF_TIMEOUT);
  VCHECK(f.sends==(where==1?0u:1u)&&f.receives==(where==1?0u:1u));
  expect_no_output(&out,&before);cases++;
 }
 Fixture f;fixture_init(&f);f.expire_after_reply=1;SfPersistVerify s={0};SfMessage m;
 request_valid(&f,&m,92,100,100+SF_PERSIST_VERIFY_NS);SfPersistSnapshot out,before;out_sentinel(&out,&before);
 VCHECK(dispatch(&f,&s,1,&m,100,1,&out)==SF_TIMEOUT&&f.sends==1&&f.receives==1);
 expect_no_output(&out,&before);cases++;
}

static void callback_results(void){
 const int errors[]={SF_BUSY,SF_BAD_SIGNATURE,1,(1<<30)};
 for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++){
  Fixture f;fixture_init(&f);f.volume_error=errors[i];SfPersistVerify s={0};SfMessage m;
  request_valid(&f,&m,100+i,100,100+SF_PERSIST_VERIFY_NS);SfPersistSnapshot out,before;out_sentinel(&out,&before);
  int expected=errors[i]<0?errors[i]:SF_BAD_FORMAT;
  VCHECK(dispatch(&f,&s,1,&m,100,1,&out)==expected&&f.volume_calls==1);
  expect_no_output(&out,&before);cases++;
 }
 Fixture f;fixture_init(&f);f.volume_mutate=1;SfPersistVerify s={0};SfMessage m;
 request_valid(&f,&m,110,100,100+SF_PERSIST_VERIFY_NS);SfPersistSnapshot out,before;out_sentinel(&out,&before);
 VCHECK(dispatch(&f,&s,1,&m,100,1,&out)==SF_STALE_HANDLE);
 expect_no_output(&out,&before);cases++;
}

static void reply_normalization(void){
 SfMessage m;SfPersistSnapshot expected;expected_snapshot(&expected);
 memset(&m,0xa5,sizeof(m));m.h.request_id=0x123456789ull;m.h.deadline_ns=500;
 persist_verify_reply(&m,0,100,&expected);
 VCHECK(m.h.size==sizeof(SfIpcHeader)&&m.h.protocol_major==1&&!m.h.protocol_minor&&
        m.h.opcode==OP_PERSIST_CANDIDATE_VERIFY&&!m.h.flags&&m.h.request_id==0x123456789ull&&
        !m.h.cap_count&&m.h.payload_bytes==sizeof(m)-sizeof(SfIpcHeader)&&m.h.deadline_ns==500);
 VCHECK(!m.status&&!m.handle&&!m.offset&&!m.mode&&m.length==sizeof(expected)&&!memcmp(m.data,&expected,sizeof(expected)));
 for(unsigned i=sizeof(expected);i<sizeof(m.data);i++)VCHECK(!m.data[i]);
 m.h.request_id=9;m.h.deadline_ns=0;persist_verify_reply(&m,SF_BUSY,100,0);
 VCHECK(m.status==SF_BUSY&&!m.length&&m.h.deadline_ns==100+SF_PERSIST_VERIFY_NS);
 VCHECK(!m.mode&&!m.handle&&!m.offset&&!m.h.cap_count);
 persist_verify_reply(&m,1,100,&expected);VCHECK(m.status==SF_BAD_FORMAT&&!m.length&&!m.data[0]);
}

int main(void){
 valid_and_one_shot();request_rejections();request_bounds_nonce_and_available();
 snapshot_and_read_limits();reply_variants();error_and_clock_paths();callback_results();reply_normalization();
 printf("PERSIST VERIFY PASS %u checks across %u cases; E1 protocol/transport callback mock only\n",checks,cases);
 return 0;
}
