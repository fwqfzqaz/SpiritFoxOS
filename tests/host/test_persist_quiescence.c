/* Actual Root client and authenticated provider parser; real volume is linked
 * from persist_volume_fixture.c. Transport corruption remains host-only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LIBSF_H
#include "sf.h"
static int64_t sf_sys(long,uint64_t,uint64_t,uint64_t);
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
unsigned persist_quiescence_volume_tests(void);
void persist_fixture_snapshot(void);
int persist_fixture_unchanged(void);
unsigned persist_fixture_brackets(void);
static unsigned checks,sends,receives,closed,variant;
static int guard_error,send_error,recv_error,semantic,real_provider;
static SfMessage wire;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"QUIESCENCE FAIL line %d variant%u: %s\n",__LINE__,variant,#c);exit(1);}}while(0)
static int64_t sf_sys(long a,uint64_t b,uint64_t c,uint64_t d){(void)a;(void)b;(void)c;(void)d;return SF_DENIED;}
static long sf_cap_close(uint64_t h){CHECK(h==0x777);closed++;return 0;}
static long sf_cap_derive(uint64_t h,uint64_t r){(void)h;(void)r;return SF_DENIED;}
static long sf_send_cap(uint32_t e,SfMessage*m,uint64_t c){(void)e;(void)m;(void)c;CHECK(0);return SF_DENIED;}
int sf_storage_quiescent(void){return guard_error;}
uint64_t sf_storage_admission_instance(void){return 0;}
uint32_t sf_storage_namespace_pid(void){return 0;}
int sf_storage_driver(uint32_t*a,uint32_t*b,uint32_t*c,uint64_t*d,int*e){(void)a;(void)b;(void)c;(void)d;(void)e;return SF_DEVICE_GONE;}
uint8_t*sf_storage_scratch(void){return 0;}
long sf_storage_load(const char*n,void*p,size_t c){(void)n;(void)p;(void)c;return SF_DEVICE_GONE;}
static long sf_send(uint32_t e,SfMessage*m){
 CHECK(e==0x101&&m->h.opcode==OP_PERSIST_QUIESCENT&&!m->handle&&!m->offset&&!m->length&&!m->mode&&!m->status&&!m->h.cap_count);
 sends++;if(send_error)return send_error;wire=*m;wire.h.flags=1;
 wire.status=real_provider?persist_metadata_dispatch(1,&wire):semantic;return 0;
}
static long sf_recv(uint32_t e,SfMessage*m,uint64_t d){
 CHECK(e==0x102&&d==sf_deadline(10000));receives++;if(recv_error)return recv_error;*m=wire;m->h.flags=12;
 switch(variant){
 case 1:return 13;case 2:m->h.request_id++;break;case 3:m->h.size--;break;
 case 4:m->h.protocol_major++;break;case 5:m->h.protocol_minor++;break;case 6:m->h.opcode++;break;
 case 7:m->h.flags=1;break;case 8:m->h.payload_bytes--;break;case 9:m->h.deadline_ns++;break;
 case 10:m->handle=1;break;case 11:m->offset=1;break;case 12:m->length=1;break;case 13:m->mode=1;break;
 case 14:m->status=1;break;case 15:m->status=1ll<<32;break;case 16:m->status=-(1ll<<32);break;
 case 17:m->status=SF_VERSION_REJECTED-1;break;case 18:m->h.cap_count=1;m->handle=0x777;break;
 case 19:m->h.cap_count=2;break;
 case 20:m->h.cap_count=1;m->handle=0x777;return 13;
 case 21:m->h.cap_count=1;m->handle=0x777;m->h.request_id++;break;
 }
 return 12;
}
static void reset(void){persist_ep=0x101;persist_reply=0x102;persist_pid=12;requests=0x300000000ull;uncertain_commit=trust_error=guard_error=send_error=recv_error=semantic=real_provider=0;variant=sends=receives=closed=0;}
static void unchanged(unsigned dirty){CHECK(!uncertain_commit&&!trust_error&&persist_fixture_unchanged()&&persist_fixture_brackets()==dirty);}
int main(void){
 unsigned volume_checks=persist_quiescence_volume_tests(),dirty=persist_fixture_brackets();
 for(unsigned i=0;i<2;i++)for(unsigned v=0;v<=21;v++){
  reset();variant=v;semantic=i?SF_BUSY:0;
  int expected=!v?semantic:(v<=2||v>=20)?SF_STALE_HANDLE:SF_BAD_FORMAT;
  CHECK(sf_persist_quiescent()==expected);CHECK(sends==1&&receives==1&&closed==(v==18||v==20||v==21));unchanged(dirty);
 }
 for(int e=SF_INVALID;e>=SF_VERSION_REJECTED;e--){reset();semantic=e;CHECK(sf_persist_quiescent()==e);unchanged(dirty);}
 reset();real_provider=1;CHECK(!sf_persist_quiescent());unchanged(dirty);
 reset();send_error=SF_WOULD_BLOCK;CHECK(sf_persist_quiescent()==SF_WOULD_BLOCK&&sends==1&&!receives);unchanged(dirty);
 reset();recv_error=SF_TIMEOUT;CHECK(sf_persist_quiescent()==SF_TIMEOUT&&sends==1&&receives==1);unchanged(dirty);
 for(unsigned i=0;i<5;i++){
  reset();int expected=SF_DEVICE_GONE;
  if(i==0)guard_error=SF_BUSY;else if(i==1)guard_error=SF_DRAINING;else if(i==2)guard_error=SF_DEVICE_GONE;else if(i==3)persist_ep=0;else{requests=UINT64_MAX;expected=SF_NO_SPACE;}
  if(i<3)expected=guard_error;CHECK(sf_persist_quiescent()==expected&&!sends&&!receives);unchanged(dirty);
 }
 for(int e=SF_INVALID;e>=SF_VERSION_REJECTED;e--){reset();trust_error=e;CHECK(sf_persist_quiescent()==e&&!sends&&!receives&&trust_error==e&&!uncertain_commit&&persist_fixture_unchanged());}
 reset();uncertain_commit=1;trust_error=SF_BAD_FORMAT;CHECK(sf_persist_quiescent()==SF_COMMIT_UNCERTAIN&&!sends&&!receives&&uncertain_commit&&trust_error==SF_BAD_FORMAT&&persist_fixture_brackets()==dirty&&persist_fixture_unchanged());
 for(unsigned v=0;v<=15;v++){
  SfMessage m;sf_message(&m,OP_PERSIST_QUIESCENT,99);m.h.flags=1;int64_t from=1;
  switch(v){
  case 1:from=2;break;case 2:m.h.flags=2;break;case 3:m.h.size--;break;case 4:m.h.protocol_major++;break;
  case 5:m.h.protocol_minor++;break;case 6:m.h.opcode=OP_PERSIST_MOUNT;break;case 7:m.h.request_id=0;break;
  case 8:m.h.cap_count=1;m.handle=0x777;break;case 9:m.h.payload_bytes--;break;case 10:m.status=1;break;
  case 11:m.handle=1;break;case 12:m.offset=1;break;case 13:m.length=1;break;case 14:m.mode=1;break;case 15:from=-1;break;
  }
  SfMessage before=m;CHECK(persist_metadata_dispatch(from,&m)==(!v?0:(v==1||v==15)?SF_DENIED:SF_INVALID));
  CHECK(!memcmp(&m,&before,sizeof(m))&&persist_fixture_unchanged());
 }
 CHECK(persist_metadata_dispatch(1,0)==SF_INVALID&&persist_metadata_dispatch(2,0)==SF_DENIED);
 printf("PERSIST QUIESCENCE PASS %u client/parser checks; %u actual volume checks\n",checks,volume_checks);return 0;
}
