#include "libsf.h"
#include "persistence.h"
#include "trust.h"
#include "txn.h"
/* sf-init side of the persistence split: every volume operation is a bounded
 * RPC to sf-persist.sfmod. The block driver endpoints are derived here with
 * RIGHT_DELEGATE and transferred once, so the service needs no root-only task
 * inspection and the init keeps its own original handles for readfs. */
static uint32_t persist_ep,persist_reply,persist_pid;
static uint64_t requests=0x300000000ull;
static long(*disk_read)(unsigned,uint64_t,void*,size_t);
static int ready(void){return persist_ep&&persist_reply&&persist_pid;}
static void hooks(SfNamespace*ns);
static int pf_call(SfMessage*m){
 if(!ready())return SF_DEVICE_GONE;
 uint64_t id=++requests;m->h.request_id=id;
 int r=sf_send(persist_ep,m);if(r)return r;
 long from=sf_recv(persist_reply,m,sf_deadline(10000));if(from<0)return(int)from;
 if(from!=(long)persist_pid||m->h.request_id!=id)return SF_STALE_HANDLE;
 return(int)m->status;
}
static int pf_send_cap(uint64_t opcode,uint32_t cap,uint64_t offset,uint32_t mode){
 if(!ready()){sf_cap_close(cap);return SF_DEVICE_GONE;}
 SfMessage m;sf_message(&m,opcode,++requests);m.offset=offset;m.mode=mode;
 int r=sf_send_cap(persist_ep,&m,cap);sf_cap_close(cap);if(r)return r;
 long from=sf_recv(persist_reply,&m,sf_deadline(10000));if(from<0)return(int)from;
 if(from!=(long)persist_pid||m.h.request_id!=requests)return SF_STALE_HANDLE;
 return(int)m.status;
}
static int attach(void){
 if(!ready())return SF_DEVICE_GONE;
 uint32_t ep=0,reply=0,pid=0;uint64_t bytes=0;int writable=0;
 if(sf_storage_driver(&ep,&reply,&pid,&bytes,&writable))return SF_DEVICE_GONE;
 int sep=(int)sf_cap_derive(ep,RIGHT_SEND|RIGHT_DELEGATE);if(sep<0)return sep;
 int r=pf_send_cap(OP_PERSIST_ATTACH_SEND,(uint32_t)sep,pid,0);if(r)return r;
 int rcap=(int)sf_cap_derive(reply,RIGHT_RECV|RIGHT_DELEGATE);if(rcap<0)return rcap;
 return pf_send_cap(OP_PERSIST_ATTACH_REPLY,(uint32_t)rcap,bytes,writable?1u:0u);
}
int sf_persist_start(SfModuleSpawn spawn){
 if(ready())return SF_BUSY;
 persist_reply=(uint32_t)sf_sys(SYS_EP_CREATE,0,0,0);if((int)persist_reply<0){int r=(int)persist_reply;persist_reply=0;return r;}
 persist_ep=(uint32_t)sf_sys(SYS_EP_CREATE,0,0,0);if((int)persist_ep<0){int r=(int)persist_ep;persist_ep=0;sf_sys(SYS_CAP_REVOKE,persist_reply,0,0);persist_reply=0;return r;}
 int pid=spawn("sf-persist.sfmod",persist_ep,RIGHT_RECV,0,0,persist_reply);
 if(pid<0){sf_sys(SYS_CAP_REVOKE,persist_ep,0,0);sf_sys(SYS_CAP_REVOKE,persist_reply,0,0);persist_ep=persist_reply=0;return pid;}
 persist_pid=(uint32_t)pid;return attach();
}
void sf_persist_attach(void){attach();}
int sf_persist_ready(void){return ready();}
void sf_persist_driver_down(void){
 if(!ready())return;SfMessage m;sf_message(&m,OP_PERSIST_DRIVER_DOWN,0);pf_call(&m);
}
static void trust_sync(void){
 if(!ready())return;uint64_t offset=0;long total=-1;
 for(;;){
  SfMessage m;sf_message(&m,OP_PERSIST_REVOKED,0);m.offset=offset;
  int r=pf_call(&m);if(r)return;
  total=(long)m.handle;if(m.length%32)return;
  for(unsigned i=0;i<m.length;i+=32)sf_trust_revoke(&sf_trust,m.data+i);
  offset+=m.length;if(offset>=(uint64_t)total)break;
 }
}
int sf_persist_mount(SfNamespace*ns){
 if(!ready())return SF_DEVICE_GONE;
 if(!sf_storage_writable())return SF_READ_ONLY;
 SfMessage m;sf_message(&m,OP_PERSIST_MOUNT,0);int r=pf_call(&m);if(r)return r;
 uint64_t notes=m.handle;
 r=sf_ns_create(ns,"/persist",NODE_DIR,0);if(r)return r;
 unsigned directory=0;for(unsigned i=0;i<SF_NS_NODE_MAX;i++)if(!strcmp(ns->nodes[i].path,"/persist"))directory=i;
 r=sf_ns_create(ns,"/persist/notes",NODE_DISK,0);
 if(r){memset(&ns->nodes[directory],0,sizeof(SfNode));return r;}
 for(unsigned i=0;i<SF_NS_NODE_MAX;i++)if(!strcmp(ns->nodes[i].path,"/persist/notes")){
  ns->nodes[i].external=UINT32_MAX;ns->nodes[i].mount_tag=1;ns->nodes[i].size=notes;}
 ns->nodes[directory].readonly=1;ns->nodes[directory].mount_tag=1;
 hooks(ns);
 trust_sync();
 return 0;
}
static long ext_read(uint64_t offset,void*out,size_t bytes){
 const uint32_t chunk=sizeof(((SfMessage*)0)->data);
 long total=0;uint8_t*dst=out;
 while(bytes){
  SfMessage m;sf_message(&m,OP_PERSIST_READ,0);m.offset=offset;m.length=bytes>chunk?chunk:(uint32_t)bytes;
  int r=pf_call(&m);if(r<0)return total?total:r;
  if(m.length>bytes)return total?total:SF_BAD_FORMAT;
  if(!m.length)break;
  memcpy(dst,m.data,m.length);total+=(long)m.length;offset+=m.length;dst+=m.length;bytes-=m.length;
  if(m.length<chunk)break;
 }
 return total;
}
static long ext_write(uint64_t offset,const void*data,size_t bytes){
 const uint32_t chunk=sizeof(((SfMessage*)0)->data);
 const uint8_t*src=data;long total=0;
 while(bytes){
  SfMessage m;sf_message(&m,OP_PERSIST_WRITE,0);m.offset=offset;m.length=bytes>chunk?chunk:(uint32_t)bytes;
  memcpy(m.data,src,m.length);int r=pf_call(&m);if(r<0)return r;
  uint32_t sent=m.length;offset+=sent;src+=sent;bytes-=sent;total+=(long)sent;
 }
 return total;
}
static long notes_read(unsigned index,uint64_t offset,void*out,size_t bytes){
 if(index!=UINT32_MAX)return disk_read?disk_read(index,offset,out,bytes):SF_DEVICE_GONE;
 if(!sf_storage_writable())return SF_DEVICE_GONE;
 return ext_read(offset,out,bytes);
}
static long notes_write(unsigned index,uint64_t offset,const void*data,size_t bytes){
 if(index!=UINT32_MAX)return SF_READ_ONLY;
 if(!sf_storage_writable())return SF_DEVICE_GONE;
 return ext_write(offset,data,bytes);
}
static int notes_truncate(unsigned index,uint64_t bytes){
 if(index!=UINT32_MAX)return SF_READ_ONLY;
 if(!sf_storage_writable())return SF_DEVICE_GONE;
 SfMessage m;sf_message(&m,OP_PERSIST_TRUNCATE,0);m.offset=bytes;return pf_call(&m);
}
/* Capture the previous external hook (readfs disk access) once, then keep the
 * persistence projection installed across unmount/mount cycles. */
static void hooks(SfNamespace*ns){
 if(ns->external_read!=notes_read)disk_read=ns->external_read;
 ns->external_read=notes_read;ns->external_write=notes_write;ns->external_truncate=notes_truncate;
}
static int stage(const uint8_t*bytes,size_t n){
 const uint64_t chunk=sizeof(((SfMessage*)0)->data);
 for(uint64_t off=0;off<n;){
  SfMessage m;sf_message(&m,OP_PERSIST_STAGE,0);m.offset=off;m.length=n-off>chunk?chunk:(uint32_t)(n-off);
  memcpy(m.data,bytes+off,m.length);int r=pf_call(&m);if(r)return r;off+=m.length;
 }
 return 0;
}
int sf_persist_install(const char*name){
 if(!ready()||!sf_storage_writable())return SF_DEVICE_GONE;
 uint8_t*snapshot=sf_storage_scratch();long n=sf_storage_load(name,snapshot,SF_STORAGE_SNAPSHOT_BYTES);if(n<0)return(int)n;
 int r=stage(snapshot,n);if(r)return r;
 SfMessage m;sf_message(&m,OP_PERSIST_INSTALL,0);return pf_call(&m);
}
int sf_persist_begin(void){
 if(!ready()||!sf_storage_writable())return SF_DEVICE_GONE;
 SfMessage m;sf_message(&m,OP_PERSIST_BEGIN,0);return pf_call(&m);
}
int sf_persist_finish(int success){
 if(!ready())return SF_DEVICE_GONE;
 SfMessage m;sf_message(&m,OP_PERSIST_FINISH,0);m.mode=success?1u:0u;return pf_call(&m);
}
int sf_persist_revoke(const char*name){
 if(!ready()||!sf_storage_writable())return SF_DEVICE_GONE;
 uint8_t*snapshot=sf_storage_scratch();long n=sf_storage_load(name,snapshot,SF_STORAGE_SNAPSHOT_BYTES);if(n<0)return(int)n;
 int r=stage(snapshot,n);if(r)return r;
 SfMessage m;sf_message(&m,OP_PERSIST_REVOKE,0);r=pf_call(&m);if(r>=0)trust_sync();
 return r;
}
static long export(void*out,size_t capacity,int preview){
 if(!ready()||!sf_storage_writable())return SF_DEVICE_GONE;
 uint64_t offset=0;long total=-1;
 for(;;){
  SfMessage m;sf_message(&m,preview?OP_PERSIST_PREVIEW:OP_PERSIST_PACKAGE,0);m.offset=offset;
  int r=pf_call(&m);if(r)return r;
  total=(long)m.handle;if(total>(long)capacity)return SF_NO_SPACE;
  if(m.length>sizeof(m.data))return SF_BAD_FORMAT;
  memcpy((uint8_t*)out+offset,m.data,m.length);
  offset+=m.length;if(offset>=(uint64_t)total)break;
  if(!m.length)return SF_BAD_FORMAT;
 }
 return total;
}
long sf_persist_package(void*out,size_t capacity){return export(out,capacity,0);}
long sf_persist_preview(void*out,size_t capacity){return export(out,capacity,1);}
void sf_persist_status(void){
 if(!ready()){sf_log("persist: UNAVAILABLE\n");return;}
 SfMessage m;sf_message(&m,OP_PERSIST_STATUS,0);pf_call(&m);
}
void sf_persist_trust_status(void){
 trust_sync();
 sf_log("trust: anchors=");sf_number(sf_trust.policy.count);sf_log(" revoked=");sf_number(sf_trust.revoked);sf_log("\n");
 static const char hex[]="0123456789abcdef";
 for(unsigned i=0;i<sf_trust.revoked;i++){sf_log("trust: revoked key=");char text[17];
  for(unsigned j=0;j<8;j++){text[j*2]=hex[sf_trust.revoked_ids[i][j]>>4];text[j*2+1]=hex[sf_trust.revoked_ids[i][j]&15];}
  text[16]=0;sf_log(text);sf_log("...\n");}
}
