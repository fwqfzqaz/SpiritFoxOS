/* E1 parser test: the snapshot producer is a mock callback; this is not volume
 * or guest evidence. The parser/reply implementation is included directly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "../../services/persist_snapshot.c"

static unsigned checks,callback_calls,callback_shape;
static int callback_result;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"snapshot parser failure line %d: %s\n",__LINE__,#c);exit(1);}}while(0)

static int snapshot_mock(SfPersistSnapshot*out){
 callback_calls++;memset(out,0,sizeof(*out));out->size=sizeof(*out);out->version=1;
 out->generation=9;out->notes_bytes=8;
 for(unsigned i=0;i<16;i++)out->volume[i]=(uint8_t)(0x10+i);
 for(unsigned i=0;i<32;i++){out->files_sha256[i]=(uint8_t)(0x40+i);out->trust_sha256[i]=(uint8_t)(0x80+i);}
 switch(callback_shape){
  case 1:out->size--;break;case 2:out->version++;break;case 3:out->generation=0;break;
  case 4:out->notes_bytes=SF_TXN_FILE_BYTES+1u;break;
 }
 return callback_result;
}
static SfMessage request(uint64_t nonce,uint64_t deadline){
 SfMessage m;memset(&m,0,sizeof(m));
 m.h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_PERSIST_SNAPSHOT,1,nonce,
                   sizeof(m)-sizeof(SfIpcHeader),0,deadline};return m;
}
static void zero_output(const SfPersistSnapshot*out){
 SfPersistSnapshot zero;memset(&zero,0,sizeof(zero));CHECK(!memcmp(out,&zero,sizeof(zero)));
}
static void bad_preserves(SfMessage*m,int64_t sender,uint64_t now,int expected){
 SfMessage before=*m;SfPersistSnapshot out;memset(&out,0xa5,sizeof(out));callback_calls=0;
 int r=persist_snapshot_dispatch(sender,m,now,snapshot_mock,&out);CHECK(r==expected);
 CHECK(callback_calls==0&&!memcmp(m,&before,sizeof(*m)));zero_output(&out);
}
static void parser_valid_and_failures(void){
 const uint64_t now=5000000000ull;SfMessage m=request(7,now+500);SfMessage before=m;
 SfPersistSnapshot out;memset(&out,0xa5,sizeof(out));callback_calls=callback_shape=callback_result=0;
 CHECK(!persist_snapshot_dispatch(1,&m,now,snapshot_mock,&out));CHECK(callback_calls==1&&!memcmp(&m,&before,sizeof(m)));
 CHECK(out.size==sizeof(out)&&out.version==1&&out.generation==9&&out.notes_bytes==8);
 for(unsigned i=0;i<16;i++)CHECK(out.volume[i]==(uint8_t)(0x10+i));
 SfPersistSnapshot saved=out;memset(&out,0xa5,sizeof(out));CHECK(!persist_snapshot_dispatch(1,&m,now,snapshot_mock,&out));
 CHECK(callback_calls==2&&!memcmp(&m,&before,sizeof(m))&&!memcmp(&out,&saved,sizeof(out)));

 m=request(8,now+1);SfMessage request_before=m;callback_result=SF_BUSY;memset(&out,0xa5,sizeof(out));
 CHECK(persist_snapshot_dispatch(1,&m,now,snapshot_mock,&out)==SF_BUSY);CHECK(!memcmp(&m,&request_before,sizeof(m)));zero_output(&out);
 m=request(8,now+1);request_before=m;callback_result=1;
 CHECK(persist_snapshot_dispatch(1,&m,now,snapshot_mock,&out)==SF_BAD_FORMAT);CHECK(!memcmp(&m,&request_before,sizeof(m)));zero_output(&out);
 m=request(8,now+1);request_before=m;callback_result=SF_VERSION_REJECTED-1;
 CHECK(persist_snapshot_dispatch(1,&m,now,snapshot_mock,&out)==SF_BAD_FORMAT);CHECK(!memcmp(&m,&request_before,sizeof(m)));zero_output(&out);
 callback_result=0;
 for(callback_shape=1;callback_shape<=4;callback_shape++){
  m=request(8,now+1);request_before=m;memset(&out,0xa5,sizeof(out));
  CHECK(persist_snapshot_dispatch(1,&m,now,snapshot_mock,&out)==SF_BAD_FORMAT);
  CHECK(!memcmp(&m,&request_before,sizeof(m)));zero_output(&out);
 }
 callback_shape=0;
}
static void malformed_requests(void){
 const uint64_t now=9000000000ull;
 for(unsigned bad=0;bad<16;bad++){
  SfMessage m=request(11,now+100);
  switch(bad){
   case 0:m.h.size--;break;case 1:m.h.protocol_major++;break;case 2:m.h.protocol_minor++;break;
   case 3:m.h.opcode=OP_PERSIST_STOP;break;case 4:m.h.flags=0;break;case 5:m.h.request_id=0;break;
   case 6:m.h.cap_count=1;break;case 7:m.h.payload_bytes--;break;case 8:m.h.deadline_ns=0;break;
   case 9:m.status=SF_BUSY;break;case 10:m.handle=1;break;case 11:m.offset=1;break;
   case 12:m.length=1;break;case 13:m.mode=1;break;case 14:m.data[0]=1;break;
   case 15:m.data[sizeof(m.data)-1]=1;break;
  }
  bad_preserves(&m,1,now,SF_INVALID);
 }
 SfMessage m=request(11,now+100);bad_preserves(&m,2,now,SF_DENIED);
 m=request(11,now);bad_preserves(&m,1,now,SF_TIMEOUT);
 m=request(11,now+3000000001ull);bad_preserves(&m,1,now,SF_INVALID);
}
static void alias_and_reply_tests(void){
 const uint64_t now=20000000000ull;SfMessage m=request(33,now+300);SfMessage before=m;
 unsigned calls=callback_calls;SfPersistSnapshot*out=(SfPersistSnapshot*)((uint8_t*)&m+offsetof(SfMessage,data));
 CHECK(persist_snapshot_dispatch(1,&m,now,snapshot_mock,out)==SF_INVALID);
 CHECK(callback_calls==calls&&!memcmp(&m,&before,sizeof(m)));

 SfPersistSnapshot snap;memset(&snap,0,sizeof(snap));snap.size=sizeof(snap);snap.version=1;snap.generation=4;
 memset(snap.files_sha256,0x5a,sizeof(snap.files_sha256));
 memset(m.data,0xa5,sizeof(m.data));m.h.flags=777;m.h.cap_count=3;m.status=1;m.handle=2;m.offset=3;m.length=4;m.mode=5;
 persist_snapshot_reply(&m,SF_OK,now,&snap);
 CHECK(m.h.size==sizeof(SfIpcHeader)&&m.h.protocol_major==1&&!m.h.protocol_minor&&m.h.opcode==OP_PERSIST_SNAPSHOT);
 CHECK(!m.h.flags&&!m.h.cap_count&&m.h.request_id==33&&m.h.payload_bytes==sizeof(m)-sizeof(SfIpcHeader));
 CHECK(m.h.deadline_ns==now+300&&!m.status&&!m.handle&&!m.offset&&m.length==sizeof(SfPersistSnapshot)&&!m.mode);
 CHECK(!memcmp(m.data,&snap,sizeof(SfPersistSnapshot)));
 for(unsigned i=sizeof(SfPersistSnapshot);i<sizeof(m.data);i++)CHECK(!m.data[i]);
 m=request(34,now);persist_snapshot_reply(&m,SF_BUSY,now,0);
 CHECK(m.status==SF_BUSY&&!m.length&&!m.mode&&m.h.deadline_ns==now+3000000000ull);
 for(unsigned i=0;i<sizeof(m.data);i++)CHECK(!m.data[i]);
 m=request(35,now+1);persist_snapshot_reply(&m,1,now,0);CHECK(m.status==SF_BAD_FORMAT);
 m=request(36,now+1);persist_snapshot_reply(&m,SF_OK,now,0);CHECK(m.status==SF_BAD_FORMAT);
 m=request(37,now+3000000001ull);persist_snapshot_reply(&m,SF_OK,now,&snap);
 CHECK(m.h.deadline_ns==now+3000000000ull&&m.length==sizeof(snap)&&!memcmp(m.data,&snap,sizeof(snap)));
}
int main(void){
 parser_valid_and_failures();malformed_requests();alias_and_reply_tests();
 printf("PERSIST SNAPSHOT PARSER PASS %u checks; parser callback mocked; no volume/guest claim\n",checks);return 0;
}
