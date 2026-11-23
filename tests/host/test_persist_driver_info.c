/* E1: the actual private INFO codec with a mocked Root/driver transport. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../services/persist_driver_info.c"

static unsigned checks,cases;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"PERSIST DRIVER INFO FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)

enum {R_GOOD,R_WRONG_PID,R_WRONG_NONCE,R_WRONG_DEADLINE,R_WRONG_OFFSET,
      R_WRONG_HANDLE,R_WRONG_LENGTH,R_BAD_SECTORS,R_BAD_SECTOR_BYTES,R_READ_ONLY,
      R_REPLY_CAP};
typedef struct {
 uint64_t now,finished,expected_deadline,expected_offset,expected_token,expected_sectors;
 uint32_t expected_pid,send_ep,reply_ep;
 int64_t send_result,receive_result,close_result,now_result,reply_sender;
 unsigned variant,sends,receives,closes,clock_calls;
 uint64_t closed[4];
 SfMessage sent;
} Harness;

static int64_t mock_send(void*context,uint32_t ep,SfMessage*m){
 Harness*h=context;h->sends++;
 CHECK(ep==h->send_ep&&m->h.size==sizeof(SfIpcHeader)&&m->h.protocol_major==1&&
       m->h.protocol_minor==0&&m->h.opcode==OP_BLOCK_INFO&&m->h.flags==0&&
       m->h.request_id&&m->h.deadline_ns==h->expected_deadline&&
       m->h.payload_bytes==sizeof(*m)-sizeof(SfIpcHeader)&&!m->h.cap_count&&
       m->offset==h->expected_offset&&m->handle==h->expected_token&&!m->length&&!m->mode&&!m->status);
 h->sent=*m;return h->send_result;
}
static int64_t mock_receive(void*context,uint32_t ep,SfMessage*m,uint64_t deadline){
 Harness*h=context;h->receives++;CHECK(ep==h->reply_ep&&deadline==h->expected_deadline);
 if(h->receive_result)return h->receive_result;
 uint64_t nonce=m->h.request_id,offset=m->offset,handle=m->handle;
 memset(m,0,sizeof(*m));
 m->h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_BLOCK_INFO,h->expected_pid,nonce,
                   sizeof(*m)-sizeof(SfIpcHeader),0,h->expected_deadline};
 m->offset=offset;m->handle=handle;m->length=sizeof(SfBlockInfo);
 SfBlockInfo info={h->expected_sectors,512,0};memcpy(m->data,&info,sizeof(info));
 if(h->variant==R_WRONG_PID)h->reply_sender=(int64_t)h->expected_pid+1;
 else h->reply_sender=h->expected_pid;
 switch(h->variant){
 case R_WRONG_NONCE:m->h.request_id++;break;
 case R_WRONG_DEADLINE:m->h.deadline_ns++;break;
 case R_WRONG_OFFSET:m->offset++;break;
 case R_WRONG_HANDLE:m->handle++;break;
 case R_WRONG_LENGTH:m->length--;break;
 case R_BAD_SECTORS:((SfBlockInfo*)m->data)->sectors++;break;
 case R_BAD_SECTOR_BYTES:((SfBlockInfo*)m->data)->sector_bytes++;break;
 case R_READ_ONLY:((SfBlockInfo*)m->data)->readonly=1;break;
 case R_REPLY_CAP:m->h.cap_count=1;m->handle=0xcafe;break;
 default:break;
 }
 return h->reply_sender;
}
static int64_t mock_close(void*context,uint64_t cap){
 Harness*h=context;unsigned i=h->closes++;if(i<4)h->closed[i]=cap;return h->close_result;
}
static int mock_now(void*context,uint64_t*out){
 Harness*h=context;h->clock_calls++;if(h->now_result)return (int)h->now_result;*out=h->finished;return 0;
}
static SfPersistDriverTransport transport(Harness*h){
 return(SfPersistDriverTransport){h,mock_send,mock_receive,mock_close,mock_now};
}
static SfPersistStage ready_stage(void){
 SfPersistStage s;memset(&s,0,sizeof(s));s.last_nonce=40;s.send=0x301;s.reply=0x302;
 s.phase=SF_PERSIST_STAGE_READY;s.binding.snapshot.size=sizeof(SfPersistSnapshot);
 s.binding.snapshot.version=1;s.binding.snapshot.generation=7;s.binding.snapshot.notes_bytes=3;
 s.binding.token=0xabc;s.binding.storage_instance=0x100000002ull;
 s.binding.capacity=(uint64_t)SF_TXN_SECTORS*512;s.binding.driver_pid=22;
 s.binding.fs_pid=21;s.binding.writable=1;s.binding.version=1;return s;
}
static SfMessage probe(const SfPersistStage*s,uint64_t now,uint64_t nonce){
 SfMessage m;memset(&m,0,sizeof(m));
 m.h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_PERSIST_CANDIDATE_DRIVER_INFO,1,nonce,
                   sizeof(m)-sizeof(SfIpcHeader),0,now+2000000000ull};
 m.offset=77;m.length=sizeof(s->binding);memcpy(m.data,&s->binding,sizeof(s->binding));return m;
}
static Harness mock_for(const SfPersistStage*s,const SfMessage*q,uint64_t now){
 Harness h;memset(&h,0,sizeof(h));h.now=now;h.finished=now+10;h.expected_deadline=q->h.deadline_ns;
 h.expected_offset=q->offset;h.expected_token=s->binding.token;h.expected_pid=s->binding.driver_pid;
 h.expected_sectors=s->binding.capacity/512;
 h.send_ep=s->send;h.reply_ep=s->reply;return h;
}
static void output_sentinel(SfPersistDriverInfo*out){memset(out,0xa5,sizeof(*out));}
static void successful_info_and_retry_refusal(void){
 uint64_t now=10000000000ull;SfPersistStage s=ready_stage();SfMessage q=probe(&s,now,41);
 Harness h=mock_for(&s,&q,now);SfPersistDriverInquiry state={40,0,0};SfPersistDriverInfo out;output_sentinel(&out);
 SfPersistDriverTransport t=transport(&h);
 CHECK(!persist_driver_info_dispatch(&state,&s,1,&q,now,1,&t,&out));
 CHECK(state.last_nonce==41&&state.attempted&&!state.fatal&&h.sends==1&&h.receives==1&&h.clock_calls==1&&
       !h.closes&&h.sent.h.request_id==q.h.request_id&&h.sent.h.deadline_ns==q.h.deadline_ns&&
       h.sent.offset==q.offset&&h.sent.handle==s.binding.token&&h.send_ep==s.send&&h.reply_ep==s.reply);
 CHECK(!memcmp(&out.binding,&s.binding,sizeof(s.binding))&&out.info.sectors==s.binding.capacity/512&&
       out.info.sector_bytes==512&&!out.info.readonly);
 SfMessage retry=probe(&s,now,42);unsigned sends=h.sends,receives=h.receives;SfPersistDriverInfo kept=out;
 CHECK(persist_driver_info_dispatch(&state,&s,1,&retry,now,1,&t,&out)==SF_DRAINING);
 CHECK(h.sends==sends&&h.receives==receives&&!memcmp(&out,&kept,sizeof(out))&&state.last_nonce==41);
 cases++;
}
static void pretransport_rejections(void){
 const uint64_t now=20000000000ull;SfPersistStage base=ready_stage();SfMessage q=probe(&base,now,41);
 for(unsigned kind=0;kind<10;kind++){
  SfPersistStage s=base;SfMessage bad=q;int64_t sender=1;uint64_t at=now;int available=1;int expected=SF_BAD_FORMAT;
  switch(kind){
  case 0:sender=2;expected=SF_DENIED;break;
  case 1:bad.h.size--;break;
  case 2:bad.h.protocol_major=2;break;
  case 3:bad.h.flags=0;break;
  case 4:bad.mode=1;break;
  case 5:bad.offset=0;break;
  case 6:bad.length--;break;
  case 7:bad.data[sizeof(SfPersistStageBinding)]=1;break;
  case 8:at=bad.h.deadline_ns;expected=SF_TIMEOUT;break;
  case 9:available=0;expected=SF_DRAINING;break;
  }
  Harness h=mock_for(&s,&bad,now);SfPersistDriverTransport t=transport(&h);
  SfPersistDriverInquiry state={40,0,0};SfPersistDriverInfo out;output_sentinel(&out);SfPersistDriverInfo before=out;
  CHECK(persist_driver_info_dispatch(&state,&s,sender,&bad,at,available,&t,&out)==expected);
  CHECK(!h.sends&&!h.receives&&!h.clock_calls&&!memcmp(&out,&before,sizeof(out))&&state.last_nonce==40&&!state.attempted);
  cases++;
 }
 for(unsigned kind=0;kind<4;kind++){
  SfPersistStage s=base;SfMessage bad=q;
  if(kind==0)s.phase=SF_PERSIST_STAGE_FAILED;
  if(kind==1)s.send=0;
  if(kind==2)s.binding.capacity++;
  if(kind==3)bad.h.request_id=40;
  Harness h=mock_for(&s,&bad,now);SfPersistDriverTransport t=transport(&h);
  SfPersistDriverInquiry state={40,0,0};SfPersistDriverInfo out;output_sentinel(&out);SfPersistDriverInfo before=out;
  int expected=kind==3?SF_STALE_HANDLE:kind==0?SF_DRAINING:SF_BAD_FORMAT;
  CHECK(persist_driver_info_dispatch(&state,&s,1,&bad,now,1,&t,&out)==expected);
  CHECK(!h.sends&&!h.receives&&!memcmp(&out,&before,sizeof(out))&&state.last_nonce==40&&!state.attempted);
  cases++;
 }
}
static void reply_refusals_preserve_output(void){
 const uint64_t now=30000000000ull;const unsigned variants[]={R_WRONG_PID,R_WRONG_NONCE,R_WRONG_DEADLINE,
  R_WRONG_OFFSET,R_WRONG_HANDLE,R_WRONG_LENGTH,R_BAD_SECTORS,R_BAD_SECTOR_BYTES,R_READ_ONLY};
 for(unsigned i=0;i<sizeof(variants)/sizeof(variants[0]);i++){
  SfPersistStage s=ready_stage();SfMessage q=probe(&s,now,41);Harness h=mock_for(&s,&q,now);
  h.variant=variants[i];SfPersistDriverTransport t=transport(&h);SfPersistDriverInquiry state={40,0,0};
  SfPersistDriverInfo out;output_sentinel(&out);SfPersistDriverInfo before=out;
  int expected=variants[i]==R_WRONG_PID||variants[i]==R_WRONG_NONCE?SF_STALE_HANDLE:SF_BAD_FORMAT;
  CHECK(persist_driver_info_dispatch(&state,&s,1,&q,now,1,&t,&out)==expected);
  CHECK(state.last_nonce==41&&state.attempted&&!state.fatal&&h.sends==1&&h.receives==1&&
        !memcmp(&out,&before,sizeof(out))&&!h.closes);
  cases++;
 }
 for(unsigned kind=0;kind<3;kind++){
  SfPersistStage s=ready_stage();SfMessage q=probe(&s,now,41);Harness h=mock_for(&s,&q,now);
  if(kind==0)h.send_result=SF_TIMEOUT;
  if(kind==1)h.receive_result=SF_TIMEOUT;
  if(kind==2){h.finished=q.h.deadline_ns;}
  SfPersistDriverTransport t=transport(&h);SfPersistDriverInquiry state={40,0,0};
  SfPersistDriverInfo out;output_sentinel(&out);SfPersistDriverInfo before=out;
  CHECK(persist_driver_info_dispatch(&state,&s,1,&q,now,1,&t,&out)==SF_TIMEOUT);
  CHECK(state.last_nonce==41&&state.attempted&&h.sends==1&&
        h.receives==(kind==0?0u:1u)&&!memcmp(&out,&before,sizeof(out)));
  cases++;
 }
}
static void receipt_close_and_fatal(void){
 const uint64_t now=40000000000ull;SfPersistStage s=ready_stage();SfMessage q=probe(&s,now,41);
 const int64_t close_results[]={0,SF_STALE_HANDLE,SF_DENIED};
 for(unsigned fatal=0;fatal<3;fatal++){
  SfMessage bad=q;bad.h.cap_count=1;bad.handle=0xcafe;Harness h=mock_for(&s,&bad,now);
  h.close_result=close_results[fatal];SfPersistDriverTransport t=transport(&h);
  SfPersistDriverInquiry state={40,0,0};SfPersistDriverInfo out;output_sentinel(&out);SfPersistDriverInfo before=out;
  CHECK(persist_driver_info_dispatch(&state,&s,99,&bad,now,1,&t,&out)==SF_BAD_FORMAT);
  CHECK(h.closes==1&&h.closed[0]==0xcafe&&!h.sends&&!h.receives&&state.last_nonce==40&&
        state.fatal==(fatal==2)&&!memcmp(&out,&before,sizeof(out)));
  if(fatal==2){SfMessage retry=q;retry.h.request_id=42;
   CHECK(persist_driver_info_dispatch(&state,&s,1,&retry,now,1,&t,&out)==SF_DRAINING&&h.sends==0);}
  cases++;
 }
 {
  Harness h=mock_for(&s,&q,now);h.variant=R_REPLY_CAP;h.close_result=SF_DENIED;
  SfPersistDriverTransport t=transport(&h);SfPersistDriverInquiry state={40,0,0};
  SfPersistDriverInfo out;output_sentinel(&out);SfPersistDriverInfo before=out;
  CHECK(persist_driver_info_dispatch(&state,&s,1,&q,now,1,&t,&out)==SF_BAD_FORMAT);
  CHECK(state.fatal&&state.attempted&&state.last_nonce==41&&h.closes==1&&h.closed[0]==0xcafe&&
        !memcmp(&out,&before,sizeof(out)));
  cases++;
 }
}
int main(void){
 successful_info_and_retry_refusal();pretransport_rejections();reply_refusals_preserve_output();
 receipt_close_and_fatal();
 printf("PERSIST DRIVER INFO PASS: %u checks/%u cases; mocked transport only\n",checks,cases);return 0;
}
