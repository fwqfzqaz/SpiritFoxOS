#include "persist_snapshot.h"
#include "txn.h"
static uint64_t limit(uint64_t now){return now>UINT64_MAX-3000000000ull?UINT64_MAX:now+3000000000ull;}
static int spans(uintptr_t p,size_t n,uintptr_t*end){if(n>UINTPTR_MAX-p)return 0;*end=p+n;return 1;}
int persist_snapshot_dispatch(int64_t sender,const SfMessage*m,uint64_t now,SfPersistSnapshotGet get,SfPersistSnapshot*out){
 if(!m||!out||!get)return SF_INVALID;
 uintptr_t me,oe,ms=(uintptr_t)m,os=(uintptr_t)out;
 if(!spans(ms,sizeof(*m),&me)||!spans(os,sizeof(*out),&oe)||(ms<oe&&os<me))return SF_INVALID;
 memset(out,0,sizeof(*out));
 if(sender!=1)return SF_DENIED;
 if(m->h.size!=sizeof(SfIpcHeader)||m->h.protocol_major!=1||m->h.protocol_minor||m->h.opcode!=OP_PERSIST_SNAPSHOT||
    m->h.flags!=1||!m->h.request_id||m->h.cap_count||m->h.payload_bytes!=sizeof(*m)-sizeof(SfIpcHeader)||
    !m->h.deadline_ns||m->status||m->handle||m->offset||m->length||m->mode)return SF_INVALID;
 for(unsigned i=0;i<sizeof(m->data);i++)if(m->data[i])return SF_INVALID;
 if(m->h.deadline_ns<=now)return SF_TIMEOUT;
 if(m->h.deadline_ns>limit(now))return SF_INVALID;
 SfPersistSnapshot local;memset(&local,0,sizeof(local));int r=get(&local);
 if(r)return r>=SF_VERSION_REJECTED&&r<0?r:SF_BAD_FORMAT;
 if(local.size!=sizeof(local)||local.version!=1||!local.generation||local.notes_bytes>SF_TXN_FILE_BYTES)return SF_BAD_FORMAT;
 *out=local;return 0;
}
void persist_snapshot_reply(SfMessage*m,int status,uint64_t now,const SfPersistSnapshot*snapshot){
 if(!m)return;
 if(status<SF_VERSION_REJECTED||status>0||(!status&&!snapshot))status=SF_BAD_FORMAT;
 uint64_t id=m->h.request_id,deadline=m->h.deadline_ns;if(deadline<=now||deadline>limit(now))deadline=limit(now);
 memset(m,0,sizeof(*m));m->h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_PERSIST_SNAPSHOT,0,id,
                                      sizeof(*m)-sizeof(SfIpcHeader),0,deadline};
 m->status=status;if(!status){m->length=sizeof(*snapshot);memcpy(m->data,snapshot,sizeof(*snapshot));}
}
