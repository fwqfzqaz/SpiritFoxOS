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
static void sf_log(const char*);
static void sf_number(uint64_t);
#include "../../services/persist_client.c"

static unsigned checks,sends,recvs,closes,loads,stages,mutations,dirty_begins,dirty_ends,dirty_active;
static unsigned corrupt,sys_calls,sys_fail,revokes,spawn_calls;
static int send_error,recv_error,query_status,mount_status,mutation_status,device_up=1,driver_up,spawn_result=12;
static uint32_t list_bytes;
static uint8_t list_data[SF_TRUST_MAX*32],scratch[SF_STORAGE_SNAPSHOT_BYTES],app_id[32];
static SfMessage reply;
static char logs[4096];static size_t logged;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"TRUST SYNC FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static void sf_log(const char*s){size_t n=strlen(s);CHECK(n<sizeof(logs)-logged);memcpy(logs+logged,s,n+1);logged+=n;}
static void sf_number(uint64_t n){char s[32];snprintf(s,sizeof(s),"%llu",(unsigned long long)n);sf_log(s);}
static long sf_sys(long op,uint64_t a,uint64_t b,uint64_t c){
 (void)a;(void)b;(void)c;
 if(op==SYS_EP_CREATE){sys_calls++;return sys_calls==sys_fail?SF_NO_SPACE:(long)(0x100+sys_calls);}
 if(op==SYS_CAP_REVOKE){revokes++;return 0;}return SF_DENIED;
}
static long sf_cap_close(uint64_t cap){CHECK(cap>0);closes++;return 0;}
static long sf_cap_derive(uint64_t cap,uint64_t rights){CHECK(cap>0&&(rights==(RIGHT_SEND|RIGHT_DELEGATE)||rights==(RIGHT_RECV|RIGHT_DELEGATE)));return (long)(cap+0x1000);}
int sf_storage_writable(void){return device_up;}
int sf_storage_transaction(int begin){if(begin){dirty_begins++;dirty_active++;}else{CHECK(dirty_active>0);dirty_active--;dirty_ends++;}return 0;}
int sf_storage_driver(uint32_t*ep,uint32_t*back,uint32_t*pid,uint64_t*bytes,int*write){if(!driver_up)return SF_DEVICE_GONE;*ep=0x501;*back=0x502;*pid=33;*bytes=2097152;*write=1;return 0;}
uint8_t*sf_storage_scratch(void){return scratch;}
long sf_storage_load(const char*name,void*out,size_t cap){
 (void)name;loads++;FILE*f=fopen("build/diskhello.sfk","rb");CHECK(f);size_t n=fread(out,1,cap,f);CHECK(n>0&&fgetc(f)==EOF);fclose(f);return(long)n;
}
static long sf_send_cap(uint32_t ep,SfMessage*m,uint64_t cap){CHECK(ep==persist_ep&&cap>0);sends++;if(send_error)return send_error;reply=*m;reply.handle=0x888;return 0;}
static long sf_send(uint32_t ep,SfMessage*m){
 CHECK(ep==persist_ep&&!m->h.cap_count);sends++;if(send_error)return send_error;reply=*m;
 switch(m->h.opcode){
 case OP_PERSIST_REVOKED:reply.handle=list_bytes;reply.length=list_bytes;reply.status=query_status;memcpy(reply.data,list_data,sizeof(list_data));break;
 case OP_PERSIST_MOUNT:reply.handle=12;reply.status=mount_status;break;
 case OP_PERSIST_STAGE:stages++;break;
 case OP_PERSIST_REVOKE:case OP_PERSIST_INSTALL:mutations++;reply.status=mutation_status;break;
 case OP_PERSIST_BEGIN:case OP_PERSIST_FINISH:break;
 case OP_PERSIST_READ:reply.length=m->length>4?4:m->length;memcpy(reply.data,"note",reply.length);break;
 default:CHECK(0);
 }return 0;
}
static long sf_recv(uint32_t ep,SfMessage*m,uint64_t until){
 CHECK(ep==persist_reply&&until==sf_deadline(10000));recvs++;if(recv_error)return recv_error;*m=reply;m->h.flags=persist_pid;
 if(reply.h.opcode!=OP_PERSIST_REVOKED)return persist_pid;
 switch(corrupt){
 case 1:return persist_pid+1;case 2:m->h.request_id++;break;case 3:m->h.size--;break;case 4:m->h.protocol_major++;break;
 case 5:m->h.protocol_minor++;break;case 6:m->h.opcode++;break;case 7:m->h.flags=1;break;case 8:m->h.payload_bytes--;break;case 9:m->h.deadline_ns++;break;
 case 10:m->offset=1;break;case 11:m->mode=1;break;case 12:m->handle=0;m->length=32;memcpy(m->data,app_id,32);break;
 case 13:m->handle=32;m->length=0;break;case 14:m->handle=(SF_TRUST_MAX+1)*32;break;case 15:m->handle=33;m->length=33;break;
 case 16:m->length=UINT32_MAX;break;case 17:m->handle=UINT64_MAX;break;case 18:m->h.cap_count=1;m->handle=0x777;break;
 case 19:m->status=1ll<<32;break;case 20:m->status=-(1ll<<32);break;
 case 21:m->h.cap_count=1;m->handle=0x777;return persist_pid+1;
 }return persist_pid;
}
static void reset(void){
 persist_ep=0x101;persist_reply=0x102;persist_pid=12;requests=0x300000000ull;uncertain_commit=trust_error=0;
 sends=recvs=closes=loads=stages=mutations=dirty_begins=dirty_ends=dirty_active=0;
 corrupt=sys_calls=sys_fail=revokes=spawn_calls=0;send_error=recv_error=query_status=mount_status=mutation_status=0;device_up=1;driver_up=0;spawn_result=12;
 list_bytes=0;memset(list_data,0,sizeof(list_data));logged=0;logs[0]=0;CHECK(!sf_trust_init(&sf_trust,&sf_trust_store));
}
static void ids(unsigned count){list_bytes=count*32;for(unsigned i=0;i<count;i++)memset(list_data+i*32,(int)(i+1),32);}
static int spawn_service(const char*name,uint32_t ep,uint32_t rights,uint32_t dev,uint32_t dev_rights,uint32_t back){
 CHECK(!strcmp(name,"sf-persist.sfmod")&&ep==persist_ep&&rights==RIGHT_RECV&&!dev&&!dev_rights&&back==persist_reply);CHECK(!sf_persist_security_error());spawn_calls++;return spawn_result;
}
int main(void){
 CHECK(sf_trust_store.count>=2);sf_sha256(app_id,sf_trust_store.keys[1].public_key,32);
 for(unsigned n=0;n<=SF_TRUST_MAX;n++){
  reset();ids(n);CHECK(!trust_sync(1));CHECK(sf_trust.revoked==n&&!sf_persist_security_error()&&!sf_persist_uncertain());CHECK(sends==1&&recvs==1&&!dirty_begins&&!dirty_ends);
  for(unsigned i=0;i<n;i++)CHECK(sf_trust_revoked(&sf_trust,list_data+i*32));
  SfRuntimeTrust before=sf_trust;list_bytes=0;CHECK(!trust_sync(0)&&!memcmp(&sf_trust,&before,sizeof(before))); /* Empty is not erasure. */
 }
 reset();ids(2);memcpy(list_data+32,list_data,32);CHECK(!trust_sync(0)&&sf_trust.revoked==1); /* Existing duplicate semantics. */
 for(corrupt=1;corrupt<=21;){
  unsigned bad=corrupt;reset();corrupt=bad;CHECK(!sf_trust_revoke(&sf_trust,app_id));SfRuntimeTrust before=sf_trust;
  int expected=bad==1||bad==2||bad==21?SF_STALE_HANDLE:SF_BAD_FORMAT;
  CHECK(trust_sync(0)==expected&&sf_persist_security_error()==expected);CHECK(!memcmp(&sf_trust,&before,sizeof(before))&&!sf_persist_uncertain());
  CHECK(sends==1&&closes==(bad==18||bad==21)&&!dirty_begins&&!dirty_ends);
  corrupt=0;CHECK(!trust_sync(0)&&!sf_persist_security_error());CHECK(!memcmp(&sf_trust,&before,sizeof(before)));corrupt=bad+1;
 }
 reset();CHECK(!sf_trust_revoke(&sf_trust,app_id));SfRuntimeTrust before=sf_trust;ids(SF_TRUST_MAX);
 CHECK(trust_sync(0)==SF_NO_SPACE&&!memcmp(&sf_trust,&before,sizeof(before))&&sf_persist_security_error()==SF_NO_SPACE);
 unsigned old=sends;list_bytes=0;CHECK(trust_sync(0)==SF_NO_SPACE&&sends==old); /* No forgetting the observed capacity fault. */
 const int unavailable[]={SF_DEVICE_GONE,SF_TIMEOUT,SF_WOULD_BLOCK};
 for(unsigned i=0;i<3;i++){
  reset();CHECK(!sf_trust_revoke(&sf_trust,app_id));before=sf_trust;query_status=unavailable[i];
  CHECK(trust_sync(0)==unavailable[i]&&!sf_persist_security_error()&&!memcmp(&sf_trust,&before,sizeof(before)));
  CHECK(trust_sync(1)==unavailable[i]&&sf_persist_security_error()==unavailable[i]&&!sf_persist_uncertain());
  query_status=0;CHECK(!trust_sync(0)&&!sf_persist_security_error()&&!memcmp(&sf_trust,&before,sizeof(before)));
 }
 reset();send_error=SF_WOULD_BLOCK;CHECK(trust_sync(0)==SF_WOULD_BLOCK&&!recvs&&!sf_persist_security_error());
 reset();recv_error=SF_TIMEOUT;CHECK(trust_sync(0)==SF_TIMEOUT&&!sf_persist_security_error());
 reset();uncertain_commit=1;before=sf_trust;CHECK(trust_sync(1)==SF_COMMIT_UNCERTAIN&&sf_persist_security_error()==SF_COMMIT_UNCERTAIN&&sf_persist_uncertain()&&!sends&&!memcmp(&before,&sf_trust,sizeof(before)));
 for(unsigned bad=1;bad<=3;bad++){
  reset();persist_ep=persist_reply=persist_pid=0;if(bad<=2)sys_fail=bad;else spawn_result=SF_DENIED;
  CHECK(sf_persist_start(spawn_service)==(bad<=2?SF_NO_SPACE:SF_DENIED));CHECK(!ready()&&sf_persist_security_error()==SF_DEVICE_GONE);CHECK(revokes==(bad==1?0:bad==2?1:2));
 }
 reset();persist_ep=persist_reply=persist_pid=0;driver_up=1;CHECK(!sf_persist_start(spawn_service)&&sf_persist_security_error()==SF_DEVICE_GONE&&spawn_calls==1&&closes==2);
 SfNamespace*ns=sf_ns_default();sf_ns_init(ns,0);CHECK(!sf_persist_mount(ns)&&!sf_persist_security_error()&&!sf_persist_uncertain()&&dirty_begins==1&&dirty_ends==1&&!dirty_active);
 reset();sf_ns_init(ns,0);mount_status=SF_BAD_FORMAT;CHECK(sf_persist_mount(ns)==SF_BAD_FORMAT&&sf_persist_security_error()==SF_DEVICE_GONE&&!sf_persist_uncertain()&&!dirty_active);
 reset();sf_ns_init(ns,0);corrupt=12;CHECK(sf_persist_mount(ns)==SF_BAD_FORMAT&&sf_persist_security_error()==SF_BAD_FORMAT&&!sf_persist_uncertain()&&!dirty_active);
 reset();trust_error=SF_BAD_FORMAT;old=sends;CHECK(sf_persist_begin()==SF_BAD_FORMAT&&sf_persist_install("fixture")==SF_BAD_FORMAT&&sf_persist_revoke("fixture")==SF_BAD_FORMAT&&sends==old&&!loads);
 CHECK(!sf_persist_finish(0)&&sf_persist_security_error()==SF_BAD_FORMAT&&!dirty_active); /* Existing trial cleanup remains allowed. */
 char note[5]={0};CHECK(ext_read(0,note,4)==4&&!memcmp(note,"note",4)&&sf_persist_security_error()==SF_BAD_FORMAT);
 reset();corrupt=12;before=sf_trust;CHECK(sf_persist_revoke("fixture")==SF_BAD_FORMAT&&mutations==1&&stages>=3&&!dirty_active&&!sf_persist_uncertain());
 CHECK(sf_persist_security_error()==SF_BAD_FORMAT&&!memcmp(&sf_trust,&before,sizeof(before)));old=sends;CHECK(sf_persist_begin()==SF_BAD_FORMAT&&sends==old);
 corrupt=0;list_bytes=32;memcpy(list_data,app_id,32);CHECK(!trust_sync(0)&&!sf_persist_security_error()&&sf_trust_revoked(&sf_trust,app_id));
 reset();corrupt=12;sf_persist_trust_status();char text[64];snprintf(text,sizeof(text),"trust: anchors=%u revoked=0",sf_trust_store.count);CHECK(strstr(logs,"trust: sync unavailable code=3")&&strstr(logs,text));
 corrupt=0;sf_persist_trust_status();CHECK(!sf_persist_security_error());
 printf("TRUST SYNC PASS: %u checks; actual client/crypto, atomic monotonic cache, bounded snapshot, readiness/dirty separation and caller guards\n",checks);return 0;
}
