/* E1: actual Root WRITE client with a bounded, authenticated fake transport.
 * A malformed ACK can follow a committed write; never replay it or release
 * its admitted DIRTY lease. This is not a guest/provider or hardware test. */
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

enum { GUARD=19, LOGICAL=2*SF_MSG_BYTES+17, PADDED=5*SF_MSG_BYTES+17 };
static struct {uint8_t before[GUARD],data[PADDED],after[GUARD];} source,source_before;
static struct {uint8_t before[GUARD],data[LOGICAL],after[GUARD];} committed;
static unsigned checks,sends,recvs,closed,begins,ends,dirty,mutate_call,stop_after;
static unsigned variant,malformed_cases,known_errors,unknown_cases,transport_cases;
static int semantic,send_error,recv_error,begin_error,end_error;
static uint64_t base,previous_id,observed_offset[4];
static uint32_t observed_length[4];
static size_t logical;
static SfMessage reply;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"PERSIST WRITE FAIL line %d variant%u call%u: %s\n",__LINE__,variant,sends,#c);exit(1);}}while(0)
#include "storage_reply_unavailable.h"

static int64_t sf_sys(long a,uint64_t b,uint64_t c,uint64_t d){(void)a;(void)b;(void)c;(void)d;return SF_DENIED;}
static long sf_cap_close(uint64_t cap){CHECK(cap==0x777);closed++;return 0;}
static long sf_cap_derive(uint64_t cap,uint64_t rights){(void)cap;(void)rights;return SF_DENIED;}
static long sf_send_cap(uint32_t ep,SfMessage*m,uint64_t cap){(void)ep;(void)m;(void)cap;CHECK(0);return SF_DENIED;}
int sf_storage_writable(void){return 1;}
int sf_storage_quiescent(void){return dirty?SF_BUSY:0;}
uint64_t sf_storage_admission_instance(void){return 0;}
uint32_t sf_storage_namespace_pid(void){return 0;}
int sf_storage_transaction(int begin){
 if(begin){CHECK(!dirty);begins++;if(begin_error)return begin_error;dirty=1;}
 else{CHECK(dirty==1);ends++;if(end_error)return end_error;dirty=0;}
 return 0;
}
int sf_storage_driver(uint32_t*a,uint32_t*b,uint32_t*c,uint64_t*d,int*e){(void)a;(void)b;(void)c;(void)d;(void)e;return SF_DEVICE_GONE;}
uint8_t*sf_storage_scratch(void){return 0;}
long sf_storage_load(const char*n,void*p,size_t c){(void)n;(void)p;(void)c;return SF_DEVICE_GONE;}

static long sf_send(uint32_t ep,SfMessage*m){
 CHECK(ep==0x101&&dirty==1&&m->h.opcode==OP_PERSIST_WRITE&&!m->h.cap_count);
 CHECK(m->h.request_id>previous_id);previous_id=m->h.request_id;sends++;
 /* ext_write copies before SEND. The baseline probes deliberately allocate
  * five chunks, so their one stopped extra send cannot read out of bounds,
  * including after unsigned remaining-byte underflow. Never loop forever. */
 CHECK(sends<=stop_after+1);
 if(sends<=4){observed_offset[sends-1]=m->offset;observed_length[sends-1]=m->length;}
 if(sends>stop_after)return SF_WOULD_BLOCK;
 if(send_error&&sends==mutate_call)return send_error;
 CHECK(m->offset>=base&&m->offset-base<=logical);
 size_t off=(size_t)(m->offset-base),remaining=logical-off;
 CHECK(m->length==(remaining>SF_MSG_BYTES?SF_MSG_BYTES:remaining));
 CHECK(!m->handle&&!m->mode&&!m->status&&m->length<=sizeof(m->data));
 CHECK(!memcmp(m->data,source.data+off,m->length));
 reply=*m;reply.h.flags=12;reply.status=m->length;
 if(sends==mutate_call&&semantic)reply.status=semantic;
 /* A successful/unknown provider may already have committed the admitted
  * bytes. Semantic known rejections leave this request's destination alone. */
 if(reply.status>=0||reply.status==SF_COMMIT_UNCERTAIN)memcpy(committed.data+off,m->data,m->length);
 return 0;
}
static long sf_recv(uint32_t ep,SfMessage*m,uint64_t deadline){
 CHECK(ep==0x102&&deadline==sf_deadline(10000));recvs++;
 if(sends==mutate_call&&recv_error)return recv_error;
 *m=reply;if(sends!=mutate_call)return 12;
 switch(variant){
 case 1:m->length=0;break;case 2:m->length--;break;case 3:m->length++;break;
 case 4:m->length=UINT32_MAX;break;case 5:m->offset--;break;case 6:m->offset++;break;
 case 7:m->handle=1;break;case 8:m->handle=UINT64_MAX;break;
 case 9:m->mode=1;break;case 10:m->mode=UINT32_MAX;break;
 case 11:m->status=0;break;case 12:m->status--;break;case 13:m->status++;break;
 case 14:m->status=1ll<<32;break;case 15:m->status=-(1ll<<32);break;
 case 16:m->status=SF_VERSION_REJECTED-1;break;
 case 17:m->h.size--;break;case 18:m->h.protocol_major++;break;case 19:m->h.protocol_minor++;break;
 case 20:m->h.opcode++;break;case 21:m->h.flags++;break;case 22:m->h.payload_bytes--;break;
 case 23:m->h.deadline_ns++;break;case 24:m->h.cap_count=1;m->handle=0x777;break;
 case 25:m->h.cap_count=2;break;case 26:return 13;case 27:m->h.request_id++;break;
 case 28:m->h.cap_count=1;m->handle=0x777;return 13;
 case 29:m->h.cap_count=1;m->handle=0x777;m->h.request_id++;break;
 /* The excessive baseline ACK remains below the padded allocation and the
  * next SEND stops before a second receive can manufacture more progress. */
 case 30:m->length=3*SF_MSG_BYTES;break;
 }
 return 12;
}
static void reset(uint64_t offset,size_t bytes){
 persist_ep=0x101;persist_reply=0x102;persist_pid=12;requests=0x300000000ull;
 uncertain_commit=trust_error=0;sends=recvs=closed=begins=ends=dirty=0;
 variant=0;mutate_call=1;stop_after=3;semantic=send_error=recv_error=begin_error=end_error=0;
 base=offset;logical=bytes;previous_id=0;
 memset(observed_offset,0,sizeof(observed_offset));memset(observed_length,0,sizeof(observed_length));
 memset(&source,0xa5,sizeof(source));for(size_t i=0;i<sizeof(source.data);i++)source.data[i]=(uint8_t)(i*29+i/257);
 source_before=source;memset(&committed,0xcc,sizeof(committed));
}
static void canaries(void){
 CHECK(!memcmp(&source,&source_before,sizeof(source)));
 for(unsigned i=0;i<GUARD;i++)CHECK(committed.before[i]==0xcc&&committed.after[i]==0xcc);
}
static void committed_prefix(size_t n){
 CHECK(n<=logical&&!memcmp(committed.data,source.data,n));
 for(size_t i=n;i<sizeof(committed.data);i++)CHECK(committed.data[i]==0xcc);
 canaries();
}
static void latched(unsigned completed,unsigned caps){
 CHECK(sf_persist_uncertain()&&sf_persist_security_error()==SF_COMMIT_UNCERTAIN&&dirty==1);
 CHECK(sends==completed+1&&recvs==sends&&begins==sends&&ends==completed&&closed==caps);
 unsigned old_sends=sends,old_begins=begins,old_ends=ends;uint64_t untouched=UINT64_MAX;
 CHECK(ext_write(base,source.data,logical)==SF_COMMIT_UNCERTAIN);
 CHECK(sf_persist_notes_size(&untouched)==SF_COMMIT_UNCERTAIN&&untouched==UINT64_MAX);
 CHECK(sf_persist_quiescent()==SF_COMMIT_UNCERTAIN&&sf_persist_finish(0)==SF_COMMIT_UNCERTAIN);
 sf_persist_driver_down();
 CHECK(sends==old_sends&&begins==old_begins&&ends==old_ends&&dirty==1&&sf_persist_uncertain());
}
static void valid_tests(void){
 const size_t sizes[]={0,1,17,SF_MSG_BYTES-1,SF_MSG_BYTES,SF_MSG_BYTES+1,LOGICAL};
 const uint64_t offsets[]={0,123,(1ull<<33)+11};
 for(unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);i++)for(unsigned j=0;j<sizeof(sizes)/sizeof(*sizes);j++){
  reset(offsets[i],sizes[j]);CHECK(ext_write(base,source.data,logical)==(long)logical);
  unsigned chunks=(unsigned)((logical+SF_MSG_BYTES-1)/SF_MSG_BYTES);
  CHECK(sends==chunks&&recvs==chunks&&begins==chunks&&ends==chunks&&!dirty&&!sf_persist_uncertain()&&!closed);
  committed_prefix(logical);
 }
}
static void malformed_tests(void){
 for(unsigned at=1;at<=3;at++)for(unsigned bad=1;bad<=30;bad++){
  reset((1ull<<33)+11,LOGICAL);variant=bad;mutate_call=at;
  int expected=bad>=26&&bad<=29?SF_STALE_HANDLE:SF_BAD_FORMAT;
  CHECK(ext_write(base,source.data,logical)==expected);
  latched(at-1,bad==24||bad==28||bad==29);committed_prefix(at==3?logical:(size_t)at*SF_MSG_BYTES);malformed_cases++;
 }
 /* Known negative ACKs still require exact echoed fields. They cannot be
  * used to discard an admitted lease when the reply shape is malformed. */
 for(unsigned at=1;at<=3;at++)for(unsigned bad=1;bad<=10;bad++){
  reset(123,LOGICAL);mutate_call=at;variant=bad;semantic=SF_NO_SPACE;
  CHECK(ext_write(base,source.data,logical)==SF_BAD_FORMAT);latched(at-1,0);
  committed_prefix((size_t)(at-1)*SF_MSG_BYTES);malformed_cases++;
 }
}
static void semantic_tests(void){
 for(unsigned at=1;at<=3;at++)for(int error=SF_INVALID;error>=SF_VERSION_REJECTED;error--){
  reset(123,LOGICAL);mutate_call=at;semantic=error;
  CHECK(ext_write(base,source.data,logical)==error);
  if(error==SF_COMMIT_UNCERTAIN){
   latched(at-1,0);committed_prefix(at==3?logical:(size_t)at*SF_MSG_BYTES);unknown_cases++;
  }else{
   CHECK(sends==at&&recvs==at&&begins==at&&ends==at&&!dirty&&!sf_persist_uncertain());
   committed_prefix((size_t)(at-1)*SF_MSG_BYTES);known_errors++;
   /* A known rejection leaves the transport usable without clearing a latch. */
   semantic=0;stop_after=sends+3;CHECK(ext_write(base,source.data,logical)==(long)logical);
   CHECK(!dirty&&!sf_persist_uncertain()&&begins==ends);committed_prefix(logical);
  }
 }
 for(unsigned at=1;at<=3;at++)for(unsigned bad=1;bad<=10;bad++){
  reset(123,LOGICAL);mutate_call=at;variant=bad;semantic=SF_COMMIT_UNCERTAIN;
  CHECK(ext_write(base,source.data,logical)==SF_BAD_FORMAT);latched(at-1,0);
  committed_prefix(at==3?logical:(size_t)at*SF_MSG_BYTES);unknown_cases++;
 }
}
static void transport_tests(void){
 for(unsigned at=1;at<=3;at++){
  reset(123,LOGICAL);mutate_call=at;send_error=SF_WOULD_BLOCK;
  CHECK(ext_write(base,source.data,logical)==SF_WOULD_BLOCK);
  CHECK(sends==at&&recvs==at-1&&begins==at&&ends==at&&!dirty&&!sf_persist_uncertain());
  committed_prefix((size_t)(at-1)*SF_MSG_BYTES);transport_cases++;
  reset(123,LOGICAL);mutate_call=at;recv_error=SF_TIMEOUT;
  CHECK(ext_write(base,source.data,logical)==SF_TIMEOUT);latched(at-1,0);
  committed_prefix(at==3?logical:(size_t)at*SF_MSG_BYTES);transport_cases++;
 }
 reset(123,LOGICAL);begin_error=SF_BUSY;CHECK(ext_write(base,source.data,logical)==SF_BUSY);
 CHECK(!sends&&!recvs&&begins==1&&!ends&&!dirty&&!sf_persist_uncertain());committed_prefix(0);transport_cases++;
 reset(123,LOGICAL);end_error=SF_BUSY;CHECK(ext_write(base,source.data,logical)==SF_BAD_FORMAT);
 CHECK(sends==1&&recvs==1&&begins==1&&ends==1&&dirty==1&&sf_persist_uncertain());
 committed_prefix(SF_MSG_BYTES);transport_cases++;
}
static int probe(void){
 printf("{\"evidence\":\"E1 bounded host fake transport\",\"allocation_bytes\":%u,\"logical_bytes\":%u,\"cases\":[",PADDED,LOGICAL);
 for(unsigned at=1;at<=2;at++)for(unsigned kind=0;kind<2;kind++){
  reset(123,LOGICAL);mutate_call=at;stop_after=at;variant=kind?30:1;
  long result=ext_write(base,source.data,logical);canaries();
  CHECK(sends<=at+1);unsigned extra=sends>at;
  printf("%s{\"chunk\":%u,\"reply_length\":%u,\"result\":%ld,\"sends\":%u,\"extra_send\":%s,\"extra_offset\":%llu,\"extra_length\":%u,\"dirty\":%u,\"uncertain\":%d}",at==1&&!kind?"":",",at,kind?3*SF_MSG_BYTES:0,result,sends,extra?"true":"false",(unsigned long long)(extra?observed_offset[at]:0),extra?observed_length[at]:0,dirty,sf_persist_uncertain());
 }
 puts("],\"canaries_unchanged\":true,\"bounded\":true}");return 0;
}
int main(int argc,char**argv){
 if(argc==2&&!strcmp(argv[1],"--probe"))return probe();
 CHECK(argc==1);valid_tests();malformed_tests();semantic_tests();transport_tests();
 printf("PERSIST WRITE REPLY PASS %u checks; %u malformed cases, %u known errors, %u unknown cases, %u transport cases; E1 host only\n",checks,malformed_cases,known_errors,unknown_cases,transport_cases);return 0;
}
