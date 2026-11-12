#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LIBSF_H
#include "sf.h"
static long sf_sys(long,uint64_t,uint64_t,uint64_t);
static long sf_send(uint32_t,SfMessage*);
static long sf_recv(uint32_t,SfMessage*,uint64_t);
static long sf_cap_close(uint64_t);
static long sf_cap_derive(uint64_t,uint64_t);
static long sf_send_cap(uint32_t,SfMessage*,uint64_t);
static uint64_t sf_deadline(unsigned ms){return 10000000ull+(uint64_t)ms*1000000;}
static void sf_message(SfMessage*m,unsigned op,uint64_t id){memset(m,0,sizeof(*m));m->h.size=40;m->h.protocol_major=1;m->h.opcode=op;m->h.request_id=id;m->h.payload_bytes=sizeof(*m)-40;m->h.deadline_ns=sf_deadline(3000);}
static void sf_log(const char*s){(void)s;}
static void sf_number(uint64_t n){(void)n;}
#include "../../services/persist_client.c"
#include "persist_server.h"
#include "../../services/namespace_state.h"
unsigned persist_volume_tests(void);
void persist_fixture_snapshot(void);
int persist_fixture_unchanged(void);
void persist_fixture_device(int);
unsigned persist_fixture_brackets(void);
static unsigned checks,sends,recvs,closes,corrupt;
static int send_error,recv_error,semantic;
static SfMessage reply;
static SfNamespace fresh,copy;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"PERSIST METADATA FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static long sf_sys(long op,uint64_t a,uint64_t b,uint64_t c){(void)op;(void)a;(void)b;(void)c;return SF_DENIED;}
static long sf_cap_close(uint64_t h){CHECK(h==0x777);closes++;return 0;}
static long sf_cap_derive(uint64_t h,uint64_t r){(void)h;(void)r;return SF_DENIED;}
static long sf_send_cap(uint32_t h,SfMessage*m,uint64_t c){(void)h;(void)m;(void)c;return SF_DENIED;}
int sf_storage_driver(uint32_t*a,uint32_t*b,uint32_t*c,uint64_t*d,int*e){(void)a;(void)b;(void)c;(void)d;(void)e;return SF_DEVICE_GONE;}
uint8_t*sf_storage_scratch(void){return 0;}
long sf_storage_load(const char*n,void*p,size_t c){(void)n;(void)p;(void)c;return SF_DEVICE_GONE;}
static long sf_send(uint32_t h,SfMessage*m){
 CHECK(h==0x101&&(m->h.opcode==OP_PERSIST_NOTES_SIZE||m->h.opcode==OP_PERSIST_TRUNCATE)&&!m->h.cap_count);sends++;if(send_error)return send_error;
 reply=*m;reply.h.flags=1;reply.status=m->h.opcode==OP_PERSIST_TRUNCATE?0:persist_metadata_dispatch(1,&reply);if(semantic){reply.status=semantic;reply.handle=0;}return 0;
}
static long sf_recv(uint32_t h,SfMessage*m,uint64_t deadline){
 CHECK(h==0x102&&deadline==sf_deadline(10000));recvs++;if(recv_error)return recv_error;
 *m=reply;m->h.flags=12;
 switch(corrupt){
 case 1:return 13;case 2:m->h.request_id++;break;case 3:m->h.size--;break;case 4:m->h.protocol_major++;break;
 case 5:m->h.protocol_minor++;break;case 6:m->h.opcode++;break;case 7:m->h.flags=1;break;case 8:m->h.payload_bytes--;break;
 case 9:m->h.deadline_ns++;break;case 10:m->offset=1;break;case 11:m->length=1;break;case 12:m->mode=1;break;
 case 13:m->status=1;break;case 14:m->status=-22;break;case 15:m->status=-(1ll<<32);break;case 16:m->handle=16385;break;
 case 17:m->handle=1ull<<32;break;case 18:m->h.cap_count=1;m->handle=0x777;break;case 19:m->status=SF_DEVICE_GONE;break;
 }return 12;
}
static void reset(void){persist_ep=0x101;persist_reply=0x102;persist_pid=12;uncertain_commit=0;corrupt=0;send_error=recv_error=semantic=0;}
int main(void){
 checks=persist_volume_tests();reset();
 /* Request validation runs against the actual mounted TRYING volume. */
 SfMessage m,before;sf_message(&m,OP_PERSIST_NOTES_SIZE,7);m.h.flags=1;before=m;
 int64_t actors[]={0,2,UINT32_MAX,1ll<<32,INT64_MAX};
 for(unsigned i=0;i<sizeof(actors)/sizeof(*actors);i++){CHECK(persist_metadata_dispatch(actors[i],&m)==SF_DENIED);CHECK(!memcmp(&m,&before,sizeof(m)));}
 CHECK(persist_metadata_dispatch(1,0)==SF_INVALID);
 for(unsigned i=0;i<14;i++){
  m=before;switch(i){case 0:m.h.size--;break;case 1:m.h.protocol_major++;break;case 2:m.h.protocol_minor++;break;
   case 3:m.h.opcode++;break;case 4:m.h.flags=2;break;case 5:m.h.request_id=0;break;case 6:m.h.cap_count=1;break;
   case 7:m.h.payload_bytes--;break;case 8:m.status=1;break;case 9:m.handle=1;break;case 10:m.offset=1;break;
   case 11:m.length=1;break;case 12:m.mode=1;break;case 13:m.h.cap_count=2;break;}
  SfMessage old=m;CHECK(persist_metadata_dispatch(1,&m)==SF_INVALID);CHECK(!memcmp(&m,&old,sizeof(m)));
 }
 CHECK(persist_fixture_unchanged());
 for(unsigned round=0;round<1000;round++){
  uint64_t n=UINT64_MAX;CHECK(!sf_persist_notes_size(&n)&&n==12);
  sf_ns_init(&fresh,0);CHECK(!sf_persist_reproject(&fresh));
  int h=sf_ns_open(&fresh,42,"/","/persist/notes",OPEN_READ);CHECK(h>0);
  SfFileStat stat;CHECK(!sf_ns_stat(&fresh,42,h,&stat)&&stat.type==NODE_DISK&&stat.size==12);CHECK(!sf_ns_close(&fresh,42,h));
  copy=fresh;CHECK(sf_persist_reproject(&fresh)==SF_BUSY&&!memcmp(&fresh,&copy,sizeof(fresh)));
 }
 CHECK(persist_fixture_unchanged());
 for(corrupt=1;corrupt<=19;corrupt++){
  uint64_t n=UINT64_MAX;unsigned oldclose=closes;CHECK(sf_persist_notes_size(&n)==(corrupt<=2?SF_STALE_HANDLE:SF_BAD_FORMAT));CHECK(n==UINT64_MAX);
  CHECK(closes==oldclose+(corrupt==18));sf_ns_init(&fresh,0);copy=fresh;
  CHECK(sf_persist_reproject(&fresh)==(corrupt<=2?SF_STALE_HANDLE:SF_BAD_FORMAT)&&!memcmp(&fresh,&copy,sizeof(fresh)));
 }
 reset();for(int e=SF_INVALID;e>=SF_VERSION_REJECTED;e--){semantic=e;uint64_t n=UINT64_MAX;CHECK(sf_persist_notes_size(&n)==e&&n==UINT64_MAX);}semantic=0;
 send_error=SF_WOULD_BLOCK;uint64_t n=UINT64_MAX;unsigned calls=recvs;CHECK(sf_persist_notes_size(&n)==SF_WOULD_BLOCK&&n==UINT64_MAX&&recvs==calls);send_error=0;
 recv_error=SF_TIMEOUT;CHECK(sf_persist_notes_size(&n)==SF_TIMEOUT&&n==UINT64_MAX);recv_error=0;
 calls=sends;CHECK(sf_persist_notes_size(0)==SF_INVALID&&sf_persist_reproject(0)==SF_INVALID&&sends==calls);
 requests=UINT64_MAX;CHECK(sf_persist_notes_size(&n)==SF_NO_SPACE&&sends==calls&&requests==UINT64_MAX);requests=0x300000000ull;
 persist_pid=0;CHECK(sf_persist_notes_size(&n)==SF_DEVICE_GONE&&n==UINT64_MAX);reset();
 persist_fixture_device(0);sf_ns_init(&fresh,0);copy=fresh;CHECK(sf_persist_notes_size(&n)==SF_DEVICE_GONE&&n==UINT64_MAX);CHECK(sf_persist_reproject(&fresh)==SF_DEVICE_GONE&&!memcmp(&fresh,&copy,sizeof(fresh)));persist_fixture_device(1);
 CHECK(persist_fixture_unchanged());
 /* Root's production dirty bracket surrounds writes. Only an authenticated
  * completion or a refused SEND releases it; uncertain in-flight work pins
  * recovery and blocks further volume use. The ACK itself is modeled here. */
 reset();unsigned brackets=persist_fixture_brackets();sf_message(&m,OP_PERSIST_TRUNCATE,0);
 CHECK(!pf_call(&m)&&persist_fixture_brackets()==brackets+2&&!sf_persist_uncertain());
 reset();brackets=persist_fixture_brackets();send_error=SF_WOULD_BLOCK;sf_message(&m,OP_PERSIST_TRUNCATE,0);
 CHECK(pf_call(&m)==SF_WOULD_BLOCK&&persist_fixture_brackets()==brackets+2&&!sf_persist_uncertain());
 for(unsigned bad=0;bad<3;bad++){
  reset();brackets=persist_fixture_brackets();if(!bad)recv_error=SF_TIMEOUT;else corrupt=bad==1?1:3;
  sf_message(&m,OP_PERSIST_TRUNCATE,0);CHECK(pf_call(&m)==(!bad?SF_TIMEOUT:bad==1?SF_STALE_HANDLE:SF_BAD_FORMAT));
  CHECK(sf_persist_uncertain()&&persist_fixture_brackets()==brackets+1);
  unsigned sent_before=sends;uint64_t untouched=UINT64_MAX;CHECK(sf_persist_notes_size(&untouched)==SF_COMMIT_UNCERTAIN&&untouched==UINT64_MAX&&sends==sent_before);
 }
 printf("Persistence metadata tests passed: %u assertions; real TRYING volume, no I/O/cache/trust changes, authenticated codec and atomic fresh-tree reprojection\n",checks);return 0;
}
