#include "libsf.h"
#include "persistence.h"
#include "trust.h"
#include "txn.h"
#include "persist_candidate.h"
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
static uint64_t fence_sequence,load_sequence,load_owner;
static struct {
 uint64_t token,storage_instance,capacity;
 uint32_t pid,ep,reply,fs_pid,driver_pid,driver_ep,driver_reply;
 int writable;
 SfRuntimeTrust trust;
} admission_fence;
static SfPersistStopStatus terminal_stop;
static int terminal_locked(void){
 return admission_fence.token&&terminal_stop.token==admission_fence.token&&
        terminal_stop.phase!=SF_PERSIST_TERMINAL_ACTIVE&&terminal_stop.phase!=SF_PERSIST_TERMINAL_REJECTED;
}
int sf_persist_fenced(void){return admission_fence.token!=0;}
int sf_persist_load_begin(uint64_t*token){
 if(!token)return SF_INVALID;
 if(sf_persist_fenced())return SF_DRAINING;
 if(load_owner)return SF_BUSY;
 if(load_sequence==UINT64_MAX)return SF_NO_SPACE;
 load_owner=++load_sequence;*token=load_owner;return 0;
}
int sf_persist_load_end(uint64_t token){
 if(!token||token!=load_owner)return SF_STALE_HANDLE;
 load_owner=0;return 0;
}
static long(*disk_read)(unsigned,uint64_t,void*,size_t);
static int ready(void){return persist_ep&&persist_reply&&persist_pid;}
static int provider_alive(void){int64_t status=sf_sys(SYS_TASK_STATUS,persist_pid,0,0);return status==SF_TASK_RUNNING;}
static void hooks(SfNamespace*ns);
static int transient(int r){return r==SF_DEVICE_GONE||r==SF_TIMEOUT||r==SF_WOULD_BLOCK;}
static int pf_call(SfMessage*m){
 if(!ready())return SF_DEVICE_GONE;
 if(uncertain_commit)return SF_COMMIT_UNCERTAIN;
 unsigned opcode=m->h.opcode;
 if(terminal_locked()||(sf_persist_fenced()&&opcode!=OP_PERSIST_QUIESCENT))return SF_DRAINING;
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
     /* A WRITE completion authenticates the exact admitted request, even
      * for a known error. Reject malformed echoes before releasing DIRTY. */
     (opcode==OP_PERSIST_WRITE&&(m->length!=requested||m->offset!=offset||m->handle||m->mode||
       (m->status>=0&&m->status!=requested)))||
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
 if(opcode==OP_PERSIST_QUIESCENT&&(m->handle||m->offset||m->length||m->mode))return SF_BAD_FORMAT;
 return(int)m->status;
}
static int pf_send_cap(uint64_t opcode,uint32_t cap,uint64_t offset,uint32_t mode){
 if(sf_persist_fenced()){sf_cap_close(cap);return SF_DRAINING;}
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
 if(sf_persist_fenced())return SF_DRAINING;
 if(!ready())return SF_DEVICE_GONE;
 uint32_t ep=0,reply=0,pid=0;uint64_t bytes=0;int writable=0;
 if(sf_storage_driver(&ep,&reply,&pid,&bytes,&writable))return SF_DEVICE_GONE;
 int sep=(int)sf_cap_derive(ep,RIGHT_SEND|RIGHT_DELEGATE);if(sep<0)return sep;
 int r=pf_send_cap(OP_PERSIST_ATTACH_SEND,(uint32_t)sep,pid,0);if(r)return r;
 int rcap=(int)sf_cap_derive(reply,RIGHT_RECV|RIGHT_DELEGATE);if(rcap<0)return rcap;
 return pf_send_cap(OP_PERSIST_ATTACH_REPLY,(uint32_t)rcap,bytes,writable?1u:0u);
}
int sf_persist_start(SfModuleSpawn spawn){
 if(sf_persist_fenced())return SF_DRAINING;
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
int sf_persist_quiescent(void){
 int r=sf_persist_security_error();if(r)return r;
 r=sf_storage_quiescent();if(r)return r;
 SfMessage m;sf_message(&m,OP_PERSIST_QUIESCENT,0);return pf_call(&m);
}
static int fence_storage_context(void){
 int r=sf_persist_security_error();if(r)return r;
 r=sf_storage_quiescent();if(r)return r;
 uint32_t ep=0,reply=0,pid=0;uint64_t bytes=0;int writable=0;
 if(sf_storage_driver(&ep,&reply,&pid,&bytes,&writable)||!writable)return SF_DEVICE_GONE;
 if(sf_storage_namespace_pid()!=admission_fence.fs_pid||sf_storage_admission_instance()!=admission_fence.storage_instance||
    pid!=admission_fence.driver_pid||ep!=admission_fence.driver_ep||reply!=admission_fence.driver_reply||
    bytes!=admission_fence.capacity||writable!=admission_fence.writable)return SF_STALE_HANDLE;
 if(memcmp(&sf_trust,&admission_fence.trust,sizeof(sf_trust)))return SF_STALE_HANDLE;
 return 0;
}
static int fence_bindings(void){
 int r=sf_persist_security_error();if(r)return r;
 if(!ready()||persist_pid!=admission_fence.pid||persist_ep!=admission_fence.ep||persist_reply!=admission_fence.reply)return SF_DEVICE_GONE;
 return fence_storage_context();
}
static int fence_context(void){int r=fence_bindings();return r?r:provider_alive()?0:SF_DEVICE_GONE;}
int sf_persist_fence_cancel(uint64_t token){
 if(!token||token!=admission_fence.token)return SF_STALE_HANDLE;
 if(terminal_locked())return SF_DRAINING;
 int r=fence_context();if(r)return r;
 memset(&admission_fence,0,sizeof(admission_fence));return 0;
}
int sf_persist_fence_begin(uint64_t*token){
 if(!token)return SF_INVALID;
 if(sf_persist_fenced())return SF_BUSY;
 *token=0;
 int r=sf_persist_security_error();if(r)return r;
 if(load_owner)return SF_BUSY;
 if(!ready()||!provider_alive())return SF_DEVICE_GONE;
 r=sf_storage_quiescent();if(r)return r;
 uint64_t instance=sf_storage_admission_instance();if(!instance)return SF_DEVICE_GONE;
 uint32_t ep=0,reply=0,pid=0;uint64_t bytes=0;int writable=0;
 if(sf_storage_driver(&ep,&reply,&pid,&bytes,&writable)||!writable)return SF_DEVICE_GONE;
 if(fence_sequence==UINT64_MAX)return SF_NO_SPACE;
 admission_fence.token=++fence_sequence;admission_fence.storage_instance=instance;
 admission_fence.pid=persist_pid;admission_fence.ep=persist_ep;admission_fence.reply=persist_reply;
 admission_fence.fs_pid=sf_storage_namespace_pid();admission_fence.driver_pid=pid;
 admission_fence.driver_ep=ep;admission_fence.driver_reply=reply;admission_fence.capacity=bytes;
 admission_fence.writable=writable;admission_fence.trust=sf_trust;*token=admission_fence.token;
 memset(&terminal_stop,0,sizeof(terminal_stop));terminal_stop.token=*token;
 terminal_stop.storage_instance=instance;terminal_stop.capacity=bytes;terminal_stop.writable=writable;
 terminal_stop.pid=persist_pid;terminal_stop.ep=persist_ep;terminal_stop.reply=persist_reply;
 terminal_stop.fs_pid=admission_fence.fs_pid;terminal_stop.driver_pid=pid;
 terminal_stop.driver_ep=ep;terminal_stop.driver_reply=reply;terminal_stop.trust=sf_trust;
 /* Publish the fence before the remote observation. No zero104 is a stop
  * permit: this scope leaves the provider alive and is explicitly cancelled. */
 r=sf_persist_quiescent();if(!r)r=fence_context();
 if(r){if(!sf_persist_fence_cancel(*token))*token=0;return r;}
 return 0;
}
/* One fixed absolute wire/death budget plus an iteration bound: even a failed
 * or frozen clock cannot make the owner poll forever. These are private C
 * implementation limits, not additions to the public wire ABI. */
static int terminal_now(uint64_t*now){
 int64_t ticks=sf_sys(SYS_TICKS,0,0,0);
 if(ticks<0||(uint64_t)ticks>(UINT64_MAX-3000000000ull)/10000000ull)return SF_BAD_FORMAT;
 *now=(uint64_t)ticks*10000000ull;return 0;
}
static int terminal_error(int64_t r){return r>=SF_VERSION_REJECTED&&r<0?(int)r:SF_BAD_FORMAT;}
static int terminal_safe_refusal(int64_t r){
 /* These decisions leave the old service operating: unsupported/invalid,
  * unauthorized, busy, expired or stale request. A volume, trust or unknown
  * completion fault is not evidence that its prior admission is healthy. */
 return r==SF_INVALID||r==SF_DENIED||r==SF_BUSY||r==SF_TIMEOUT||r==SF_STALE_HANDLE;
}
int sf_persist_fence_stop_status(uint64_t token,SfPersistStopStatus*out){
 if(!out)return SF_INVALID;
 if(!token||token!=terminal_stop.token)return SF_STALE_HANDLE;
 *out=terminal_stop;return 0;
}
static int terminal_observe(void){
 for(unsigned i=0;i<512;i++){
  terminal_stop.observations++;
  int64_t status=sf_sys(SYS_TASK_STATUS,terminal_stop.pid,0,0);terminal_stop.task_status=status;
  if(status==0){terminal_stop.death_confirmed=1;return 0;}
  /* Negative values overlap syscall failures and negative exit statuses.
   * Neither they nor a truncated/unknown wide value prove provider death. */
  if(status!=SF_TASK_RUNNING){
   if(status>0&&status<=INT32_MAX)terminal_stop.death_confirmed=1;
   terminal_stop.phase=SF_PERSIST_TERMINAL_FAILED;return SF_DEVICE_GONE;
  }
  uint64_t now=0;int r=terminal_now(&now);
  if(r){terminal_stop.phase=SF_PERSIST_TERMINAL_FAILED;return r;}
  if(now>=terminal_stop.deadline_ns)return SF_TIMEOUT;
  sf_sys(SYS_YIELD,0,0,0);
 }
 return SF_TIMEOUT;
}
int sf_persist_fence_stop(uint64_t token){
 if(!token||token!=admission_fence.token||token!=terminal_stop.token)return SF_STALE_HANDLE;
 /* The first attempt owns the terminal decision even if SEND/RECV fail.
  * A refusal can be cancelled, but is never silently retried with a new ID. */
 if(terminal_stop.request_id)return SF_DRAINING;
 int r=fence_context();if(r)return r;
 if(requests==UINT64_MAX)return SF_NO_SPACE;
 uint64_t now=0;r=terminal_now(&now);if(r)return r;
 terminal_stop.request_id=++requests;terminal_stop.deadline_ns=now+3000000000ull;
 terminal_stop.phase=SF_PERSIST_TERMINAL_REQUEST_UNKNOWN;
 SfMessage m;memset(&m,0,sizeof(m));
 m.h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_PERSIST_STOP,0,terminal_stop.request_id,
                  sizeof(m)-sizeof(SfIpcHeader),0,terminal_stop.deadline_ns};
 int failure=SF_BAD_FORMAT;
 terminal_stop.send_result=sf_send(terminal_stop.ep,&m);
 if(terminal_stop.send_result)failure=terminal_error(terminal_stop.send_result);
 else{
  int64_t from=sf_recv(terminal_stop.reply,&m,terminal_stop.deadline_ns);terminal_stop.receive_result=from;
  if(from<0)failure=terminal_error(from);
  else{
   terminal_stop.ack_status=m.status;terminal_stop.ack_mode=m.mode;
   if(m.h.cap_count==1)sf_cap_close(m.handle);
   int empty=1;for(unsigned i=0;i<sizeof(m.data);i++)if(m.data[i]){empty=0;break;}
   if(from==(int64_t)terminal_stop.pid&&m.h.size==sizeof(SfIpcHeader)&&m.h.protocol_major==1&&
      !m.h.protocol_minor&&m.h.opcode==OP_PERSIST_STOP&&m.h.flags==terminal_stop.pid&&
      m.h.request_id==terminal_stop.request_id&&!m.h.cap_count&&m.h.payload_bytes==sizeof(m)-sizeof(SfIpcHeader)&&
      m.h.deadline_ns==terminal_stop.deadline_ns&&!m.handle&&!m.offset&&!m.length&&empty&&
      m.mode==(m.status==0?1u:0u)&&
      m.status>=SF_VERSION_REJECTED&&m.status<=0){
    terminal_stop.ack_authenticated=1;terminal_stop.ack_status=m.status;
    if(m.status){
     terminal_stop.request_error=(int)m.status;
     r=fence_context();terminal_stop.phase=!r&&terminal_safe_refusal(m.status)?SF_PERSIST_TERMINAL_REJECTED:SF_PERSIST_TERMINAL_FAILED;
     return r?r:(int)m.status;
    }
    terminal_stop.phase=SF_PERSIST_TERMINAL_ACCEPTED_WAIT;failure=0;
   }
  }
 }
 terminal_stop.request_error=failure;
 r=terminal_observe();terminal_stop.observation_error=r;if(r)return r;
 if(!terminal_stop.ack_authenticated){terminal_stop.phase=SF_PERSIST_TERMINAL_FAILED;return failure;}
 /* Only normal death after authenticated acceptance can retire the old tuple.
  * Keep the original fence snapshot intact; old cancel is never activation. */
 r=fence_bindings();if(r){terminal_stop.phase=SF_PERSIST_TERMINAL_FAILED;return r;}
 terminal_stop.revoke_ep_result=sf_sys(SYS_CAP_REVOKE,terminal_stop.ep,0,0);
 if(!terminal_stop.revoke_ep_result)persist_ep=0;
 terminal_stop.revoke_reply_result=sf_sys(SYS_CAP_REVOKE,terminal_stop.reply,0,0);
 if(!terminal_stop.revoke_reply_result)persist_reply=0;
 if(terminal_stop.revoke_ep_result||terminal_stop.revoke_reply_result){
  terminal_stop.phase=SF_PERSIST_TERMINAL_FAILED;
  return terminal_error(terminal_stop.revoke_ep_result?terminal_stop.revoke_ep_result:terminal_stop.revoke_reply_result);
 }
 terminal_stop.retired=1;terminal_stop.phase=SF_PERSIST_TERMINAL_STOPPED_CLOSED;return 0;
}
static int candidate_context(uint64_t token){
 if(!token||token!=admission_fence.token||token!=terminal_stop.token)return SF_STALE_HANDLE;
 if(terminal_stop.phase!=SF_PERSIST_TERMINAL_STOPPED_CLOSED)return SF_DRAINING;
 if(terminal_stop.ack_authenticated!=1||terminal_stop.ack_mode!=1||terminal_stop.ack_status||
    terminal_stop.death_confirmed!=1||terminal_stop.task_status||terminal_stop.retired!=1||
    terminal_stop.revoke_ep_result||terminal_stop.revoke_reply_result||persist_ep||persist_reply||
    persist_pid!=terminal_stop.pid||terminal_stop.pid!=admission_fence.pid||
    terminal_stop.ep!=admission_fence.ep||terminal_stop.reply!=admission_fence.reply||
    terminal_stop.storage_instance!=admission_fence.storage_instance||terminal_stop.capacity!=admission_fence.capacity||
    terminal_stop.fs_pid!=admission_fence.fs_pid||terminal_stop.driver_pid!=admission_fence.driver_pid||
    terminal_stop.driver_ep!=admission_fence.driver_ep||terminal_stop.driver_reply!=admission_fence.driver_reply||
    terminal_stop.writable!=admission_fence.writable||
    memcmp(&terminal_stop.trust,&admission_fence.trust,sizeof(sf_trust)))return SF_STALE_HANDLE;
 int r=fence_storage_context();if(r)return r;
 /* A copied status record is not ownership. Re-observe the exact old PID;
  * no truncated sentinel, missing process or abnormal status is accepted. */
 return sf_sys(SYS_TASK_STATUS,terminal_stop.pid,0,0)==0?0:SF_DEVICE_GONE;
}
int sf_persist_fence_inspect(uint64_t token,SfPersistCandidateProbe probe,void*context,SfPersistCandidateImage*out){
 if(!out)return SF_INVALID;
 memset(out,0,sizeof(*out));if(!probe)return SF_INVALID;
 int r=candidate_context(token);if(r)return r;
 SfPersistCandidateImage staged;memset(&staged,0,sizeof(staged));
 r=probe(&terminal_stop.trust,&staged,context);if(r)return r<0?r:SF_BAD_FORMAT;
 SfArguments empty;memset(&empty,0,sizeof(empty));
 if(!staged.bytes||staged.bytes>SF_PACKAGE_MAX||staged.plan.size!=sizeof(staged.plan)||!staged.plan.count||staged.plan.count>8||
    staged.plan.image_base!=SF_BASE||staged.plan.grant_manager||staged.plan.grant_service||
    staged.plan.service_rights||staged.plan.grant_device||staged.plan.device_rights||staged.plan.grant_bundle||
    memcmp(&staged.plan.arguments,&empty,sizeof(empty)))return SF_BAD_FORMAT;
 r=candidate_context(token);if(r)return r;
 *out=staged;return 0;
}
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
 if(sf_persist_fenced())return SF_DRAINING;
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
 if(sf_persist_fenced())return SF_DRAINING;
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
  uint32_t sent=m.length;memcpy(m.data,src,sent);int r=pf_call(&m);if(r<0)return r;
  /* Progress follows our admitted chunk, never a mutable reply field. */
  offset+=sent;src+=sent;bytes-=sent;total+=(long)sent;
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
