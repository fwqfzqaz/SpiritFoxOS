#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../services/persist_idle.h"

static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"persist idle failure line %d: %s\n",__LINE__,#x);exit(1);}}while(0)

static SfMessage request(uint32_t opcode,uint64_t nonce,uint64_t deadline){
 SfMessage m;memset(&m,0,sizeof(m));
 m.h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,opcode,1,nonce,
                   sizeof(m)-sizeof(SfIpcHeader),0,deadline};
 return m;
}
static void unchanged(const SfPersistIdle*a,const SfPersistIdle*b,const SfMessage*m,const SfMessage*n){
 CHECK(!memcmp(a,b,sizeof(*a)));CHECK(!memcmp(m,n,sizeof(*m)));
}
static void rejected_unchanged(SfPersistIdle*s,SfMessage*m,int64_t sender,uint64_t now,int unbound,int expected){
 SfPersistIdle before=*s;SfMessage copy=*m;
 CHECK(persist_idle_dispatch(s,sender,m,now,unbound)==expected);
 unchanged(s,&before,m,&copy);
}

static void malformed_and_auth_tests(void){
 const uint64_t now=10000000000ull;SfPersistIdle state={0};SfMessage m;
 m=request(OP_PERSIST_CANDIDATE_PROBE,1,now+1);rejected_unchanged(&state,&m,2,now,1,SF_DENIED);
 for(unsigned i=0;i<11;i++){
  m=request(OP_PERSIST_CANDIDATE_PROBE,1,now+100);
  switch(i){
   case 0:m.h.size--;break;case 1:m.h.protocol_major++;break;case 2:m.h.protocol_minor=1;break;
   case 3:m.h.opcode=OP_PERSIST_STOP;break;case 4:m.h.flags=0;break;case 5:m.h.request_id=0;break;
   case 6:m.h.cap_count=1;break;case 7:m.h.payload_bytes--;break;case 8:m.status=SF_BUSY;break;
   case 9:m.data[sizeof(m.data)-1]=1;break;case 10:m.handle=1;break;
  }
  rejected_unchanged(&state,&m,1,now,1,SF_INVALID);
 }
 m=request(OP_PERSIST_CANDIDATE_PROBE,1,now+100);m.offset=1;rejected_unchanged(&state,&m,1,now,1,SF_INVALID);
 m=request(OP_PERSIST_CANDIDATE_PROBE,1,now+100);m.length=1;rejected_unchanged(&state,&m,1,now,1,SF_INVALID);
 m=request(OP_PERSIST_CANDIDATE_PROBE,1,now+100);m.mode=1;rejected_unchanged(&state,&m,1,now,1,SF_INVALID);
 m=request(OP_PERSIST_CANDIDATE_PROBE,1,now);rejected_unchanged(&state,&m,1,now,1,SF_TIMEOUT);
 m=request(OP_PERSIST_CANDIDATE_PROBE,1,now+SF_PERSIST_IDLE_NS+1);rejected_unchanged(&state,&m,1,now,1,SF_INVALID);
 m=request(OP_PERSIST_CANDIDATE_PROBE,1,now+1);CHECK(!persist_idle_dispatch(&state,1,&m,now,1));
 SfPersistIdle before=state;SfMessage replay=request(OP_PERSIST_CANDIDATE_DISCARD,1,now+2);
 rejected_unchanged(&state,&replay,1,now,1,SF_STALE_HANDLE);
 CHECK(state.last_nonce==before.last_nonce&&state.entered==1&&!state.discarded);
}

static void transition_tests(void){
 const uint64_t now=UINT64_MAX-1000000000ull;SfPersistIdle s={0};SfMessage m;
 /* Saturated upper deadline still allows a future deadline through UINT64_MAX. */
 m=request(OP_PERSIST_CANDIDATE_PROBE,5,UINT64_MAX);
 CHECK(!persist_idle_dispatch(&s,1,&m,now,1));CHECK(s.last_nonce==5&&s.entered==1&&!s.discarded);
 m=request(OP_PERSIST_CANDIDATE_PROBE,6,UINT64_MAX);
 CHECK(persist_idle_dispatch(&s,1,&m,now,1)==SF_BUSY);CHECK(s.last_nonce==6&&s.entered==1&&!s.discarded);
 m=request(OP_PERSIST_CANDIDATE_DISCARD,7,UINT64_MAX);
 CHECK(!persist_idle_dispatch(&s,1,&m,now,1));CHECK(s.last_nonce==7&&s.entered==1&&s.discarded==1);
 m=request(OP_PERSIST_CANDIDATE_DISCARD,8,UINT64_MAX);
 CHECK(persist_idle_dispatch(&s,1,&m,now,1)==SF_DRAINING);CHECK(s.last_nonce==7);

 SfPersistIdle not_ready={0};m=request(OP_PERSIST_CANDIDATE_PROBE,1,200);
 CHECK(persist_idle_dispatch(&not_ready,1,&m,100,0)==SF_BUSY);
 CHECK(not_ready.last_nonce==1&&!not_ready.entered&&!not_ready.discarded);
 m=request(OP_PERSIST_CANDIDATE_PROBE,2,200);
 CHECK(!persist_idle_dispatch(&not_ready,1,&m,100,1));
 SfPersistIdle no_entry={0};m=request(OP_PERSIST_CANDIDATE_DISCARD,3,200);
 CHECK(persist_idle_dispatch(&no_entry,1,&m,100,1)==SF_INVALID);
 CHECK(no_entry.last_nonce==3&&!no_entry.entered&&!no_entry.discarded);
}

static void reply_tests(void){
 const uint64_t now=4000000000ull;SfMessage m=request(OP_PERSIST_CANDIDATE_PROBE,19,now+500);
 m.h.flags=77;m.h.cap_count=9;m.status=SF_BUSY;m.handle=5;m.offset=6;m.length=7;m.mode=8;
 memset(m.data,0xa5,sizeof(m.data));persist_idle_reply(&m,SF_BUSY,now,1);
 CHECK(m.h.size==sizeof(SfIpcHeader)&&m.h.protocol_major==1&&!m.h.protocol_minor);
 CHECK(m.h.opcode==OP_PERSIST_CANDIDATE_PROBE&&!m.h.flags&&m.h.request_id==19);
 CHECK(m.h.payload_bytes==sizeof(m)-sizeof(SfIpcHeader)&&!m.h.cap_count&&m.h.deadline_ns==now+500);
 CHECK(m.status==SF_BUSY&&!m.handle&&!m.offset&&!m.length&&!m.mode);
 for(unsigned i=0;i<sizeof(m.data);i++)CHECK(!m.data[i]);
 m=request(OP_PERSIST_CANDIDATE_DISCARD,20,now);
 persist_idle_reply(&m,SF_OK,now,1);CHECK(m.h.opcode==OP_PERSIST_CANDIDATE_DISCARD&&m.h.deadline_ns==now+SF_PERSIST_IDLE_NS&&m.mode==1);
 m=request(OP_PERSIST_CANDIDATE_DISCARD,21,now+SF_PERSIST_IDLE_NS+1);
 persist_idle_reply(&m,SF_OK,now,1);CHECK(m.h.deadline_ns==now+SF_PERSIST_IDLE_NS&&m.mode==1);
 m=request(OP_PERSIST_CANDIDATE_PROBE,22,now+1);
 persist_idle_reply(&m,SF_OK,now,0);CHECK(!m.mode);
 m=request(OP_PERSIST_STOP,23,now+1);
 persist_idle_reply(&m,SF_OK,now,1);CHECK(m.h.opcode==OP_PERSIST_STOP&&m.mode==1);
}

int main(void){
 malformed_and_auth_tests();transition_tests();reply_tests();
 printf("PERSIST IDLE PARSER PASS %u checks; pure parser/reply E1; no signature, runtime-service, or guest claim\n",checks);
 return 0;
}
