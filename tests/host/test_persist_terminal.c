/* E1: actual production parser/receive loop, with explicit transport and
 * cached-volume substitutes. This is not provider-death or block-I/O proof. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LIBSF_H
#include "sf.h"
static int64_t sf_send(uint32_t,SfMessage*);
static int64_t sf_recv(uint32_t,SfMessage*,uint64_t);
static int64_t sf_cap_close(uint64_t);
static int64_t sf_cap_derive(uint64_t h,uint64_t rights){(void)h;(void)rights;return SF_DENIED;}
static uint64_t sf_ticks(void);
#include "../../services/persist_terminal.c"

static unsigned checks,quiescent_calls,ordinary_calls,writes,notes_calls;
static int quiet;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"PERSIST TERMINAL FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
int persist_quiescent(void){quiescent_calls++;return quiet;}
int persist_notes_size(uint64_t*n){notes_calls++;*n=0x123456789ull;return 0;}
static SfMessage request(uint64_t nonce){
 SfMessage m={0};m.h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_PERSIST_STOP,1,nonce,
 sizeof(m)-sizeof(SfIpcHeader),0,4000000000ull};return m;
}
static int ordinary(SfMessage*m){ordinary_calls++;if(m->h.opcode==OP_PERSIST_WRITE)writes++;return SF_NO_SPACE;}
static void parser_tests(void){
 SfPersistTerminal state={0};SfMessage valid=request(1),m;
 CHECK(persist_terminal_dispatch(0,1,&valid,1000000000ull)==SF_INVALID);
 CHECK(persist_terminal_dispatch(&state,1,0,1000000000ull)==SF_INVALID);
 const int64_t senders[]={0,2,-1,(1ll<<32)+1,INT64_MAX};
 for(unsigned i=0;i<sizeof(senders)/sizeof(*senders);i++){
  m=valid;CHECK(persist_terminal_dispatch(&state,senders[i],&m,1000000000ull)==SF_DENIED);
  CHECK(!state.last_nonce&&!state.accepted&&!quiescent_calls);
 }
 /* Mutate every authenticated header/business field, including its upper bits. */
 for(unsigned i=0;i<30;i++){
  m=valid;
  switch(i){
  case 0:m.h.size--;break;case 1:m.h.size++;break;
  case 2:m.h.protocol_major=0;break;case 3:m.h.protocol_major=2;break;
  case 4:m.h.protocol_minor=1;break;case 5:m.h.opcode=OP_PERSIST_QUIESCENT;break;
  case 6:m.h.flags=0;break;case 7:m.h.flags=2;break;case 8:m.h.flags=UINT32_MAX;break;
  case 9:m.h.request_id=0;break;case 10:m.h.cap_count=1;break;case 11:m.h.cap_count=2;break;
  case 12:m.h.payload_bytes--;break;case 13:m.h.payload_bytes++;break;
  case 14:m.h.deadline_ns=0;break;case 15:m.status=1;break;
  case 16:m.status=1ll<<32;break;case 17:m.status=-(1ll<<32);break;
  case 18:m.handle=1;break;case 19:m.handle=1ull<<32;break;
  case 20:m.offset=1;break;case 21:m.offset=1ull<<32;break;
  case 22:m.length=1;break;case 23:m.length=UINT32_MAX;break;
  case 24:m.mode=1;break;case 25:m.mode=UINT32_MAX;break;
  case 26:m.data[0]=1;break;case 27:m.data[sizeof(m.data)-1]=1;break;
  case 28:m.h.deadline_ns++;break;case 29:m.h.deadline_ns=UINT64_MAX;break;
  }
  CHECK(persist_terminal_dispatch(&state,1,&m,1000000000ull)==SF_INVALID);
  CHECK(!state.last_nonce&&!state.accepted&&!quiescent_calls);
 }
 for(unsigned i=0;i<3;i++){
  m=valid;m.h.deadline_ns=i==0?1:i==1?999999999ull:1000000000ull;
  CHECK(persist_terminal_dispatch(&state,1,&m,1000000000ull)==SF_TIMEOUT);
  CHECK(!state.last_nonce&&!quiescent_calls);
 }
 quiet=SF_BUSY;m=valid;
 CHECK(persist_terminal_dispatch(&state,1,&m,1000000000ull)==SF_BUSY);
 CHECK(state.last_nonce==1&&!state.accepted&&quiescent_calls==1);
 quiet=0;CHECK(persist_terminal_dispatch(&state,1,&m,1000000000ull)==SF_STALE_HANDLE);
 CHECK(quiescent_calls==1&&!state.accepted);
 m=request(0x100000001ull);
 CHECK(!persist_terminal_dispatch(&state,1,&m,1000000000ull));
 CHECK(state.accepted&&state.last_nonce==0x100000001ull&&quiescent_calls==2);
 CHECK(persist_terminal_dispatch(&state,1,&m,1000000000ull)==SF_DRAINING);
 CHECK(quiescent_calls==2);
 const int errors[]={SF_INVALID,SF_BUSY,SF_COMMIT_UNCERTAIN,SF_VERSION_REJECTED,1,-22,INT32_MIN,INT32_MAX};
 for(unsigned i=0;i<sizeof(errors)/sizeof(*errors);i++){
  state=(SfPersistTerminal){0};quiet=errors[i];m=valid;
  int expected=quiet>0||quiet<SF_VERSION_REJECTED?SF_BAD_FORMAT:quiet;
  CHECK(persist_terminal_dispatch(&state,1,&m,1000000000ull)==expected);
  CHECK(state.last_nonce==1&&!state.accepted);
 }
 state=(SfPersistTerminal){UINT64_MAX-1,0};quiet=SF_BUSY;m=request(UINT64_MAX);
 CHECK(persist_terminal_dispatch(&state,1,&m,1000000000ull)==SF_BUSY);
 CHECK(state.last_nonce==UINT64_MAX);
 m=request(UINT64_MAX);CHECK(persist_terminal_dispatch(&state,1,&m,1000000000ull)==SF_STALE_HANDLE);
 m=request(1);CHECK(persist_terminal_dispatch(&state,1,&m,1000000000ull)==SF_STALE_HANDLE);
 state=(SfPersistTerminal){0};quiet=0;m=request(1);m.h.deadline_ns=UINT64_MAX;
 CHECK(!persist_terminal_dispatch(&state,1,&m,UINT64_MAX-1));
 state=(SfPersistTerminal){0};CHECK(persist_terminal_dispatch(&state,1,&m,UINT64_MAX)==SF_TIMEOUT);
 state=(SfPersistTerminal){0};m=request(1);m.h.deadline_ns=1000000001ull;
 CHECK(!persist_terminal_dispatch(&state,1,&m,1000000000ull));
}

typedef struct {SfMessage message;int64_t sender;int cached;} Input;
static Input input[16];static SfMessage output[16];
static unsigned queued,received,sent,closed;static uint64_t last_closed,ticks=100;
static int64_t send_result,receive_result=SF_DEVICE_GONE;
static int64_t sf_recv(uint32_t h,SfMessage*m,uint64_t deadline){
 CHECK(h==0x101&&!deadline);CHECK(received<16);
 if(received==queued)return receive_result;
 Input*i=&input[received++];*m=i->message;quiet=i->cached;return i->sender;
}
static int64_t sf_send(uint32_t h,SfMessage*m){CHECK(h==0x102&&sent<16);output[sent++]=*m;return send_result;}
static unsigned fixture_sends;static int corrupt_accepted_status;
static int64_t fixture_send(uint32_t h,SfMessage*m){
 fixture_sends++;if(m->h.opcode==OP_PERSIST_WRITE)m->length=0;
 if(corrupt_accepted_status&&m->h.opcode==OP_PERSIST_STOP&&!m->status)m->status=SF_BUSY;
 return sf_send(h,m);
}
static int64_t sf_cap_close(uint64_t h){closed++;last_closed=h;return 0;}
static uint64_t sf_ticks(void){return ticks;}
static void reset(void){
 queued=received=sent=closed=0;quiescent_calls=ordinary_calls=writes=notes_calls=0;
 last_closed=0;ticks=100;quiet=0;send_result=0;receive_result=SF_DEVICE_GONE;fixture_sends=0;corrupt_accepted_status=0;
 memset(input,0,sizeof(input));memset(output,0,sizeof(output));
}
static void enqueue(SfMessage m,int64_t sender,int cached){CHECK(queued<16);input[queued++]=(Input){m,sender,cached};}
static SfBootArgs boot={.service=0x101,.manager=0x102};
static void ack(unsigned i,int status,uint64_t nonce,uint64_t deadline){
 SfMessage*m=&output[i];CHECK(m->h.size==40&&m->h.protocol_major==1&&!m->h.protocol_minor);
 CHECK(m->h.opcode==OP_PERSIST_STOP&&!m->h.flags&&m->h.request_id==nonce);
 CHECK(m->h.payload_bytes==sizeof(*m)-40&&!m->h.cap_count&&m->h.deadline_ns==deadline);
 CHECK(m->status==status&&!m->handle&&!m->offset&&!m->length&&m->mode==(status==0?1u:0u));
 uint8_t empty[SF_MSG_BYTES]={0};CHECK(!memcmp(m->data,empty,sizeof(empty)));
}
static void loop_tests(void){
 const int64_t send_errors[]={0,SF_TIMEOUT,SF_DEVICE_GONE,SF_WOULD_BLOCK,1,(1ll<<32)+1};
 for(unsigned i=0;i<sizeof(send_errors)/sizeof(*send_errors);i++){
  reset();send_result=send_errors[i];enqueue(request(0x100000001ull),1,0);
  SfMessage write=request(2);write.h.opcode=OP_PERSIST_WRITE;enqueue(write,1,0);
  CHECK(!persist_serve(&boot,ordinary,sf_send));CHECK(received==1&&sent==1&&!ordinary_calls&&!writes);
  CHECK(quiescent_calls==1);ack(0,0,0x100000001ull,4000000000ull);
 }
 /* A failed cached check stays live; nonce replay cannot use a later idle cache. */
 reset();enqueue(request(1),1,SF_BUSY);enqueue(request(1),1,0);
 SfMessage write=request(3);write.h.opcode=OP_PERSIST_WRITE;enqueue(write,1,0);
 enqueue(request(2),1,0);enqueue(write,1,0);
 CHECK(!persist_serve(&boot,ordinary,sf_send));CHECK(received==4&&sent==4&&writes==1&&ordinary_calls==1);
 CHECK(quiescent_calls==2);ack(0,SF_BUSY,1,4000000000ull);ack(1,SF_STALE_HANDLE,1,4000000000ull);
 CHECK(output[2].status==SF_NO_SPACE);ack(3,0,2,4000000000ull);
 /* Advisory 104 success cannot authorize STOP after a cached state change. */
 reset();SfMessage preflight=request(5);preflight.h.opcode=OP_PERSIST_QUIESCENT;
 enqueue(preflight,1,0);enqueue(request(6),1,SF_BUSY);enqueue(write,1,0);enqueue(request(7),1,0);
 CHECK(!persist_serve(&boot,ordinary,sf_send));CHECK(received==4&&quiescent_calls==3&&writes==1);
 CHECK(output[0].status==0);ack(1,SF_BUSY,6,4000000000ull);ack(3,0,7,4000000000ull);
 /* Wrong actual sender and malformed requests cannot poison the next valid nonce. */
 const int64_t forged[]={2,(1ll<<32)+1,INT64_MAX};
 for(unsigned i=0;i<sizeof(forged)/sizeof(*forged);i++){
  reset();SfMessage m=request(UINT64_MAX);m.h.cap_count=1;m.handle=0x100000077ull;
  enqueue(m,forged[i],0);enqueue(request(1),1,0);
  CHECK(!persist_serve(&boot,ordinary,sf_send));CHECK(closed==1&&last_closed==0x100000077ull);
  CHECK(received==2&&quiescent_calls==1);ack(0,SF_DENIED,UINT64_MAX,4000000000ull);ack(1,0,1,4000000000ull);
 }
 for(unsigned i=0;i<8;i++){
  reset();SfMessage m=request(UINT64_MAX);int status=SF_INVALID;
  switch(i){
  case 0:m.h.deadline_ns=0;break;case 1:m.h.deadline_ns=1;status=SF_TIMEOUT;break;
  case 2:m.h.deadline_ns=UINT64_MAX;break;case 3:m.h.cap_count=1;m.handle=0x88;break;
  case 4:m.status=1ll<<32;break;case 5:m.offset=1ull<<32;break;
  case 6:m.data[4095]=1;break;case 7:m.h.cap_count=2;break;
  }
  enqueue(m,1,0);enqueue(request(1),1,0);
  CHECK(!persist_serve(&boot,ordinary,sf_send));CHECK(received==2&&sent==2&&quiescent_calls==1);
  CHECK(closed==(i==3));ack(0,status,UINT64_MAX,4000000000ull);ack(1,0,1,4000000000ull);
 }
 /* Existing metadata and ordinary dispatch remain wired to their real paths. */
 reset();SfMessage notes=request(9);notes.h.opcode=OP_PERSIST_NOTES_SIZE;
 enqueue(notes,1,0);enqueue(write,2,0);enqueue(write,1,0);
 CHECK(persist_serve(&boot,ordinary,sf_send)==SF_DEVICE_GONE);
 CHECK(received==3&&sent==3&&notes_calls==1&&ordinary_calls==1&&writes==1);
 CHECK(output[0].status==0&&output[0].handle==0x123456789ull);
 CHECK(output[1].status==SF_DENIED&&output[2].status==SF_NO_SPACE);
 /* persist.c passes its local sf_send symbol, including fixture macro wrappers. */
 reset();write.length=17;enqueue(write,1,0);enqueue(request(1),1,0);enqueue(write,1,0);
 CHECK(!persist_serve(&boot,ordinary,fixture_send));CHECK(fixture_sends==2&&sent==2&&received==2);
 CHECK(writes==1&&output[0].length==0);ack(1,0,1,4000000000ull);
 reset();corrupt_accepted_status=1;enqueue(request(1),1,0);enqueue(write,1,0);
 CHECK(!persist_serve(&boot,ordinary,fixture_send));CHECK(received==1&&sent==1&&!writes);
 CHECK(output[0].status==SF_BUSY&&output[0].mode==1);
 reset();notes.h.cap_count=1;notes.handle=0x99;enqueue(notes,1,0);
 CHECK(persist_serve(&boot,ordinary,sf_send)==SF_DEVICE_GONE);CHECK(closed==1&&last_closed==0x99&&!notes_calls);
 CHECK(output[0].status==SF_INVALID&&!output[0].h.cap_count);
 reset();receive_result=-(1ll<<32);CHECK(persist_serve(&boot,ordinary,sf_send)==-(1ll<<32));CHECK(!sent);
 reset();CHECK(persist_serve(0,ordinary,sf_send)==SF_INVALID);CHECK(persist_serve(&boot,0,sf_send)==SF_INVALID);
 CHECK(persist_serve(&boot,ordinary,0)==SF_INVALID);
 SfBootArgs invalid=boot;invalid.service=0;CHECK(persist_serve(&invalid,ordinary,sf_send)==SF_INVALID);
 invalid=boot;invalid.manager=0;CHECK(persist_serve(&invalid,ordinary,sf_send)==SF_INVALID);CHECK(!received&&!sent);
 reset();ticks=UINT64_MAX/10000000ull+1;enqueue(request(1),1,0);
 CHECK(persist_serve(&boot,ordinary,sf_send)==SF_TIMEOUT);CHECK(received==1&&!sent&&!quiescent_calls);
}
int main(void){parser_tests();loop_tests();printf("Persist terminal E1: %u checks (actual production parser/receive loop; mocked transport/cache)\n",checks);return 0;}
