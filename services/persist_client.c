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
static int uncertain_commit;
/* Distinct from an uncertain write: a detected runtime-trust failure blocks
 * new package authority until a complete explicit synchronization succeeds. */
static int trust_error;
static long(*disk_read)(unsigned,uint64_t,void*,size_t);
static int ready(void){return persist_ep&&persist_reply&&persist_pid;}
static void hooks(SfNamespace*ns);
static int transient(int r){return r==SF_DEVICE_GONE||r==SF_TIMEOUT||r==SF_WOULD_BLOCK;}
static int pf_call(SfMessage*m){
 if(!ready())return SF_DEVICE_GONE;
 if(uncertain_commit)return SF_COMMIT_UNCERTAIN;
 unsigned opcode=m->h.opcode;
 int metadata=opcode==OP_PERSIST_NOTES_SIZE;
 int dirty=opcode==OP_PERSIST_MOUNT||opcode==OP_PERSIST_WRITE||opcode==OP_PERSIST_TRUNCATE||
           opcode==OP_PERSIST_INSTALL||opcode==OP_PERSIST_BEGIN||opcode==OP_PERSIST_FINISH||opcode==OP_PERSIST_REVOKE;
 if(requests==UINT64_MAX)return SF_NO_SPACE;
 if(dirty){int r=sf_storage_transaction(1);if(r)return r;}
 uint64_t id=++requests,deadline=m->h.deadline_ns,offset=m->offset;uint32_t requested=m->length;m->h.request_id=id;
 int r=sf_send(persist_ep,m);if(r){if(dirty&&(r>0||sf_storage_transaction(0)))uncertain_commit=1;return r>0?SF_BAD_FORMAT:r;}
 long from=sf_recv(persist_reply,m,sf_deadline(10000));if(from<0){if(dirty)uncertain_commit=1;return(int)from;}
 /* RECV installed any transferred capability before returning. None of
  * these replies consumes one, including non-mutating reads and failures. */
 if(m->h.cap_count==1)sf_cap_close(m->handle);
 if(from!=(long)persist_pid||m->h.request_id!=id){if(dirty)uncertain_commit=1;return SF_STALE_HANDLE;}
 unsigned maximum=opcode==OP_PERSIST_WRITE?sizeof(m->data):(opcode==OP_PERSIST_BEGIN||opcode==OP_PERSIST_INSTALL||opcode==OP_PERSIST_REVOKE)?1u:0u;
 if(m->h.size!=40||m->h.protocol_major!=1||m->h.protocol_minor||m->h.opcode!=opcode||
     m->h.flags!=persist_pid||m->h.cap_count||m->h.payload_bytes!=sizeof(*m)-40||m->h.deadline_ns!=deadline||
     m->status<SF_VERSION_REJECTED||m->status>(int64_t)maximum||
     (opcode==OP_PERSIST_WRITE&&m->status>=0&&m->status!=requested)||
     (opcode==OP_PERSIST_READ&&(m->length>requested||m->length>sizeof(m->data)||m->offset!=offset||m->handle||m->mode))||
     (opcode==OP_PERSIST_STAGE&&!m->status&&(m->length!=requested||m->offset!=offset||m->handle||m->mode))){
  if(dirty)uncertain_commit=1;return SF_BAD_FORMAT;
 }
 /* A service can authenticate that completion is unknown. Preserve the same
  * admitted dirty lease as a lost/malformed mutating reply; a query is not
  * permission to release it or replay the operation. */
 if(dirty&&m->status==SF_COMMIT_UNCERTAIN){uncertain_commit=1;return SF_COMMIT_UNCERTAIN;}
 if(dirty){
  if(sf_storage_transaction(0)){uncertain_commit=1;return SF_BAD_FORMAT;}
 }
 if(metadata&&(m->offset||m->length||m->mode||
    (m->status?m->handle!=0:m->handle>SF_TXN_FILE_BYTES)))return SF_BAD_FORMAT;
 return(int)m->status;
}
static int pf_send_cap(uint64_t opcode,uint32_t cap,uint64_t offset,uint32_t mode){
 if(!ready()){sf_cap_close(cap);return SF_DEVICE_GONE;}
 if(uncertain_commit||requests==UINT64_MAX){sf_cap_close(cap);return uncertain_commit?SF_COMMIT_UNCERTAIN:SF_NO_SPACE;}
 SfMessage m;sf_message(&m,opcode,++requests);m.offset=offset;m.mode=mode;
 uint64_t id=m.h.request_id,deadline=m.h.deadline_ns;
 int r=sf_send_cap(persist_ep,&m,cap);sf_cap_close(cap);if(r)return r>0?SF_BAD_FORMAT:r;
 long from=sf_recv(persist_reply,&m,sf_deadline(10000));if(from<0)return(int)from;
 if(m.h.cap_count==1)sf_cap_close(m.handle);
 if(from!=(long)persist_pid||m.h.request_id!=id)return SF_STALE_HANDLE;
 /* The ACK's handle is local to the provider when cap_count=0. Never close
  * it as a manager capability; an unexpected transferred cap was closed above. */
 if(m.h.size!=40||m.h.protocol_major!=1||m.h.protocol_minor||m.h.opcode!=opcode||
    m.h.flags!=persist_pid||m.h.cap_count||m.h.payload_bytes!=sizeof(m)-40||m.h.deadline_ns!=deadline||
    m.status<SF_VERSION_REJECTED||m.status>0||
    (!m.status&&(m.offset!=offset||m.mode!=mode||m.length)))return SF_BAD_FORMAT;
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
 persist_reply=(uint32_t)sf_sys(SYS_EP_CREATE,0,0,0);if((int)persist_reply<0){int r=(int)persist_reply;persist_reply=0;trust_error=SF_DEVICE_GONE;return r;}
 persist_ep=(uint32_t)sf_sys(SYS_EP_CREATE,0,0,0);if((int)persist_ep<0){int r=(int)persist_ep;persist_ep=0;sf_sys(SYS_CAP_REVOKE,persist_reply,0,0);persist_reply=0;trust_error=SF_DEVICE_GONE;return r;}
 int pid=spawn("sf-persist.sfmod",persist_ep,RIGHT_RECV,0,0,persist_reply);
 if(pid<0){sf_sys(SYS_CAP_REVOKE,persist_ep,0,0);sf_sys(SYS_CAP_REVOKE,persist_reply,0,0);persist_ep=persist_reply=0;trust_error=SF_DEVICE_GONE;return pid;}
 persist_pid=(uint32_t)pid;trust_error=SF_DEVICE_GONE;return attach();
}
void sf_persist_attach(void){attach();}
int sf_persist_ready(void){return ready();}
int sf_persist_uncertain(void){return uncertain_commit;}
int sf_persist_security_error(void){return uncertain_commit?SF_COMMIT_UNCERTAIN:trust_error;}
void sf_persist_driver_down(void){
 if(!ready())return;SfMessage m;sf_message(&m,OP_PERSIST_DRIVER_DOWN,0);pf_call(&m);
}
static int trust_fail(int r,int required){
 /* A last-known cache remains usable during observational unavailability.
  * Initial mount and known revoke completion require a fresh authoritative
  * snapshot; malformed replies/capacity faults are never transient. */
 if(required||!transient(r))trust_error=r;
 return r;
}
static int trust_sync(int required){
 _Static_assert(SF_TRUST_MAX*32u<=sizeof(((SfMessage*)0)->data),"revocation snapshot fits one message");
 /* A full monotonic cache cannot forget an observed overflow to accept a
  * later smaller list. This capacity fault needs a fresh trust context. */
 if(trust_error==SF_NO_SPACE)return SF_NO_SPACE;
 if(!ready())return trust_fail(SF_DEVICE_GONE,required);
 SfMessage m;sf_message(&m,OP_PERSIST_REVOKED,0);int r=pf_call(&m);if(r)return trust_fail(r,required);
 if(m.offset||m.mode||m.handle>SF_TRUST_MAX*32u||m.handle%32u||m.length!=m.handle)return trust_fail(SF_BAD_FORMAT,1);
 SfRuntimeTrust next=sf_trust;
 for(unsigned i=0;i<m.length;i+=32){r=sf_trust_revoke(&next,m.data+i);if(r)return trust_fail(r,1);}
 /* Publish only a complete monotonic union. Empty/older lists never erase
  * revocations already known by this manager incarnation. */
 sf_trust=next;trust_error=0;return 0;
}
static int project_notes(SfNamespace*ns,uint64_t notes){
 if(notes>UINT32_MAX)return SF_BAD_FORMAT;
 const SfMountEntry entries[]={
  {"/persist",NODE_DIR,1,0,0},
  {"/persist/notes",NODE_DISK,0,UINT32_MAX,(uint32_t)notes}
 };
 int r=sf_ns_mount_publish(ns,entries,2,1);if(r)return r;
 hooks(ns);
 return 0;
}
int sf_persist_mount(SfNamespace*ns){
 if(!ns)return SF_INVALID;
 if(!ready()){trust_error=SF_DEVICE_GONE;return SF_DEVICE_GONE;}
 if(!sf_storage_writable())return SF_READ_ONLY;
 SfMessage m;sf_message(&m,OP_PERSIST_MOUNT,0);int r=pf_call(&m);if(r){if(!transient(r))trust_error=SF_DEVICE_GONE;return r;}
 r=project_notes(ns,m.handle);int secure=trust_sync(1);return secure?secure:r;
}
int sf_persist_notes_size(uint64_t*out){
 if(!out)return SF_INVALID;
 SfMessage m;sf_message(&m,OP_PERSIST_NOTES_SIZE,0);int r=pf_call(&m);if(r)return r;
 *out=m.handle;return 0;
}
int sf_persist_reproject(SfNamespace*ns){
 if(!ns)return SF_INVALID;
 if(!sf_storage_writable())return SF_DEVICE_GONE;
 uint64_t notes=0;int r=sf_persist_notes_size(&notes);return r?r:project_notes(ns,notes);
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
 SfNsRead previous=sf_ns_external(ns,notes_read,notes_write,notes_truncate);
 if(previous!=notes_read)disk_read=previous;
}
static int stage(const uint8_t*bytes,size_t n){
 /* An empty upload must not turn INSTALL/REVOKE into use of an old stage. */
 if(!n)return SF_BAD_FORMAT;
 if(!bytes)return SF_INVALID;
 if(n>SF_TXN_FILE_BYTES)return SF_NO_SPACE;
 const uint64_t chunk=sizeof(((SfMessage*)0)->data);
 for(uint64_t off=0;off<n;){
  SfMessage m;sf_message(&m,OP_PERSIST_STAGE,0);m.offset=off;m.length=n-off>chunk?chunk:(uint32_t)(n-off);
  memcpy(m.data,bytes+off,m.length);int r=pf_call(&m);if(r)return r;off+=m.length;
 }
 return 0;
}
int sf_persist_install(const char*name){
 if(!ready()||!sf_storage_writable())return SF_DEVICE_GONE;
 int secure=sf_persist_security_error();if(secure)return secure;
 uint8_t*snapshot=sf_storage_scratch();long n=sf_storage_load(name,snapshot,SF_STORAGE_SNAPSHOT_BYTES);if(n<0)return(int)n;
 int r=stage(snapshot,n);if(r)return r;
 SfMessage m;sf_message(&m,OP_PERSIST_INSTALL,0);return pf_call(&m);
}
int sf_persist_begin(void){
 if(!ready()||!sf_storage_writable())return SF_DEVICE_GONE;
 int secure=sf_persist_security_error();if(secure)return secure;
 SfMessage m;sf_message(&m,OP_PERSIST_BEGIN,0);return pf_call(&m);
}
int sf_persist_finish(int success){
 if(!ready())return SF_DEVICE_GONE;
 SfMessage m;sf_message(&m,OP_PERSIST_FINISH,0);m.mode=success?1u:0u;return pf_call(&m);
}
int sf_persist_revoke(const char*name){
 if(!ready()||!sf_storage_writable())return SF_DEVICE_GONE;
 int secure=sf_persist_security_error();if(secure)return secure;
 uint8_t*snapshot=sf_storage_scratch();long n=sf_storage_load(name,snapshot,SF_STORAGE_SNAPSHOT_BYTES);if(n<0)return(int)n;
 int r=stage(snapshot,n);if(r)return r;
 SfMessage m;sf_message(&m,OP_PERSIST_REVOKE,0);r=pf_call(&m);
 /* Even an authenticated negative response can follow a policy commit.
  * Reconcile authoritative trust, preserving the original operation error.
  * An uncertain write remains latched and prevents this query from sending. */
 int sync=trust_sync(1);if(r<0)return r;return sync?sync:r;
}
static long export(void*out,size_t capacity,int preview){
 if(!ready()||!sf_storage_writable())return SF_DEVICE_GONE;
 if(!out&&capacity)return SF_INVALID;
 uint64_t offset=0,total=0;int have_total=0;
 for(;;){
  SfMessage m;sf_message(&m,preview?OP_PERSIST_PREVIEW:OP_PERSIST_PACKAGE,0);m.offset=offset;
  int r=pf_call(&m);if(r)return r;
  /* A slot is one transaction file. Authenticate the complete chunk shape
   * before touching output, and pin its total for this bounded export. */
  if(m.handle>SF_TXN_FILE_BYTES||(have_total&&m.handle!=total)||m.offset!=offset||m.mode)return SF_BAD_FORMAT;
  total=m.handle;have_total=1;if(total>capacity)return SF_NO_SPACE;
  if(offset>total)return SF_BAD_FORMAT;
  uint64_t remaining=total-offset;uint32_t chunk=remaining>sizeof(m.data)?sizeof(m.data):(uint32_t)remaining;
  if(m.length!=chunk)return SF_BAD_FORMAT;
  if(chunk)memcpy((uint8_t*)out+offset,m.data,chunk);
  offset+=chunk;if(offset==total)break;
 }
 return(long)total;
}
long sf_persist_package(void*out,size_t capacity){return export(out,capacity,0);}
long sf_persist_preview(void*out,size_t capacity){return export(out,capacity,1);}
void sf_persist_status(void){
 if(uncertain_commit){sf_log("persist: UNAVAILABLE code=");sf_number(-(int64_t)SF_COMMIT_UNCERTAIN);sf_log("\n");return;}
 if(!ready()){sf_log("persist: UNAVAILABLE\n");return;}
 SfMessage m;sf_message(&m,OP_PERSIST_STATUS,0);pf_call(&m);
 uint64_t notes=0;int r=sf_persist_notes_size(&notes);
 if(!r){sf_log("persist: notes-bytes=");sf_number(notes);sf_log("\n");}
 else{sf_log("persist: notes-unavailable code=");sf_number(-r);sf_log("\n");}
}
void sf_persist_trust_status(void){
 int r=trust_sync(0);if(r){sf_log("trust: sync unavailable code=");sf_number(r<0?-(int64_t)r:r);sf_log("\n");}
 sf_log("trust: anchors=");sf_number(sf_trust.policy.count);sf_log(" revoked=");sf_number(sf_trust.revoked);sf_log("\n");
 static const char hex[]="0123456789abcdef";
 for(unsigned i=0;i<sf_trust.revoked;i++){sf_log("trust: revoked key=");char text[17];
  for(unsigned j=0;j<8;j++){text[j*2]=hex[sf_trust.revoked_ids[i][j]>>4];text[j*2+1]=hex[sf_trust.revoked_ids[i][j]&15];}
  text[16]=0;sf_log(text);sf_log("...\n");}
}
