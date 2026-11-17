#include "persist_idle.h"

static uint64_t idle_limit(uint64_t now){
 return now>UINT64_MAX-SF_PERSIST_IDLE_NS?UINT64_MAX:now+SF_PERSIST_IDLE_NS;
}

int persist_idle_dispatch(SfPersistIdle*s,int64_t sender,SfMessage*m,
                          uint64_t now,int unbound){
 if(!s||!m)return SF_INVALID;
 if(s->discarded)return SF_DRAINING;
 if(sender!=1)return SF_DENIED;
 if(m->h.size!=sizeof(SfIpcHeader)||m->h.protocol_major!=1||m->h.protocol_minor||
    (m->h.opcode!=OP_PERSIST_CANDIDATE_PROBE&&m->h.opcode!=OP_PERSIST_CANDIDATE_DISCARD)||
    m->h.flags!=1||!m->h.request_id||m->h.cap_count||
    m->h.payload_bytes!=sizeof(*m)-sizeof(SfIpcHeader)||
    !m->h.deadline_ns||m->status||m->handle||m->offset||m->length||m->mode)
  return SF_INVALID;
 for(unsigned i=0;i<sizeof(m->data);i++)if(m->data[i])return SF_INVALID;
 if(m->h.deadline_ns<=now)return SF_TIMEOUT;
 if(m->h.deadline_ns>idle_limit(now))return SF_INVALID;
 if(m->h.request_id<=s->last_nonce)return SF_STALE_HANDLE;

 /* A fresh well-formed request consumes its nonce even when readiness or
  * one-shot state refuses the requested transition. */
 s->last_nonce=m->h.request_id;
 if(unbound!=1)return SF_BUSY;
 if(m->h.opcode==OP_PERSIST_CANDIDATE_PROBE){
  if(s->entered)return SF_BUSY;
  s->entered=1;return SF_OK;
 }
 if(!s->entered)return SF_INVALID;
 s->discarded=1;return SF_OK;
}

void persist_idle_reply(SfMessage*m,int status,uint64_t now,int accepted){
 if(!m)return;
 uint64_t nonce=m->h.request_id,deadline=m->h.deadline_ns,limit=idle_limit(now);
 if(deadline<=now||deadline>limit)deadline=limit;
 m->h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,m->h.opcode,0,nonce,
                   sizeof(*m)-sizeof(SfIpcHeader),0,deadline};
 m->status=status;m->handle=0;m->offset=0;m->length=0;
 m->mode=(accepted==1&&status==SF_OK)?1u:0u;
 for(unsigned i=0;i<sizeof(m->data);i++)m->data[i]=0;
}
