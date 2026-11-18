#include "persist_stage.h"

static uint64_t stage_limit(uint64_t now){return now>UINT64_MAX-SF_PERSIST_STAGE_NS?UINT64_MAX:now+SF_PERSIST_STAGE_NS;}
static int known_error(int64_t r){return r>=SF_VERSION_REJECTED&&r<0?(int)r:SF_BAD_FORMAT;}
static int close_receipt(SfPersistStage*s,const SfPersistStageCaps*c,uint64_t cap,int result){
 if(!cap)return result;
 int64_t r=c->close(c->context,cap);
 if(r&&r!=SF_STALE_HANDLE){s->phase=SF_PERSIST_STAGE_FAILED;s->fatal=1;return SF_BAD_FORMAT;}
 return result;
}
int persist_stage_dispatch(SfPersistStage*s,int64_t from,const SfMessage*m,uint64_t now,
                           int available,uint32_t self,const SfPersistSnapshot*expected,
                           const SfPersistStageCaps*c){
 if(!s||!m||!c||!c->derive||!c->close)return SF_INVALID;
 uint64_t incoming=m->h.cap_count==1?m->handle:0;
 int r=SF_INVALID;
 if(from!=1){r=SF_DENIED;goto done;}
 if(m->h.size!=sizeof(SfIpcHeader)||m->h.protocol_major!=1||m->h.protocol_minor||
    m->h.opcode!=OP_PERSIST_CANDIDATE_CAPS||m->h.flags!=1||!m->h.request_id||m->h.cap_count!=1||
    m->h.payload_bytes!=sizeof(*m)-sizeof(SfIpcHeader)||!m->h.deadline_ns||
    !incoming||incoming>2147483647u||m->status||m->offset||m->length!=sizeof(SfPersistStageBinding)||
    (m->mode!=SF_PERSIST_STAGE_SEND&&m->mode!=SF_PERSIST_STAGE_REPLY))goto done;
 for(unsigned i=sizeof(SfPersistStageBinding);i<sizeof(m->data);i++)if(m->data[i])goto done;
 if(m->h.deadline_ns<=now){r=SF_TIMEOUT;goto done;}
 if(m->h.deadline_ns>stage_limit(now))goto done;
 if(m->h.request_id<=s->last_nonce){r=SF_STALE_HANDLE;goto done;}
 s->last_nonce=m->h.request_id;
 if(s->phase==SF_PERSIST_STAGE_FAILED||s->phase==SF_PERSIST_STAGE_READY){r=SF_DRAINING;goto done;}
 if(available!=1||!expected){r=SF_BUSY;s->phase=SF_PERSIST_STAGE_FAILED;goto done;}
 SfPersistStageBinding binding;memcpy(&binding,m->data,sizeof(binding));
 if(binding.version!=1||!binding.token||!binding.storage_instance||binding.capacity%512||
    binding.capacity/512<SF_TXN_SECTORS||binding.writable!=1||
    self<=1||self>2147483647u||binding.driver_pid<=1||binding.driver_pid>2147483647u||
    binding.fs_pid<=1||binding.fs_pid>2147483647u||binding.driver_pid==binding.fs_pid||
    binding.driver_pid==self||binding.fs_pid==self||
    binding.snapshot.size!=sizeof(SfPersistSnapshot)||binding.snapshot.version!=1||
    !binding.snapshot.generation||binding.snapshot.notes_bytes>SF_TXN_FILE_BYTES||
    memcmp(&binding.snapshot,expected,sizeof(*expected))){r=SF_STALE_HANDLE;s->phase=SF_PERSIST_STAGE_FAILED;goto done;}
 if((m->mode==SF_PERSIST_STAGE_SEND&&s->phase!=SF_PERSIST_STAGE_NONE)||
    (m->mode==SF_PERSIST_STAGE_REPLY&&(s->phase!=SF_PERSIST_STAGE_SEND_HELD||
     memcmp(&binding,&s->binding,sizeof(binding))))){r=SF_DRAINING;s->phase=SF_PERSIST_STAGE_FAILED;goto done;}
 unsigned rights=m->mode==SF_PERSIST_STAGE_SEND?RIGHT_SEND:RIGHT_RECV;
 int64_t limited=c->derive(c->context,incoming,rights);
 if(limited<=0||limited>2147483647||(uint64_t)limited==incoming){
  r=known_error(limited);s->phase=SF_PERSIST_STAGE_FAILED;
  if(limited>2147483647||(limited>0&&(uint64_t)limited==incoming))s->fatal=1;
  goto done;
 }
 /* Retain ownership before closing the delegating receipt. CLOSE preserves
  * its ancestry to Root's retained parent; limited handles cannot delegate. */
 if(m->mode==SF_PERSIST_STAGE_SEND){s->send=(uint32_t)limited;s->binding=binding;s->phase=SF_PERSIST_STAGE_SEND_HELD;}
 else{s->reply=(uint32_t)limited;s->phase=SF_PERSIST_STAGE_READY;}
 r=0;
done:
 return close_receipt(s,c,incoming,r);
}
void persist_stage_reply(SfMessage*m,int status,uint64_t now,unsigned mode,const SfPersistStageBinding*binding){
 if(!m)return;
 if(!status&&(!binding||(mode!=SF_PERSIST_STAGE_SEND&&mode!=SF_PERSIST_STAGE_REPLY)))status=SF_INVALID;
 SfPersistStageBinding copy;if(!status)copy=*binding;
 uint64_t nonce=m->h.request_id,deadline=m->h.deadline_ns,limit=stage_limit(now);
 if(deadline<=now||deadline>limit)deadline=limit;
 m->h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_PERSIST_CANDIDATE_CAPS,0,nonce,sizeof(*m)-sizeof(SfIpcHeader),0,deadline};
 m->status=status;m->handle=m->offset=0;m->mode=status?0:mode;m->length=status?0:sizeof(*binding);
 memset(m->data,0,sizeof(m->data));if(!status)memcpy(m->data,&copy,sizeof(copy));
}
