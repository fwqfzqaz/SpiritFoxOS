#include "persist_verify.h"

static uint64_t verify_limit(uint64_t now){return now>UINT64_MAX-SF_PERSIST_VERIFY_NS?UINT64_MAX:now+SF_PERSIST_VERIFY_NS;}
static int known_error(int64_t error){return error>=SF_VERSION_REJECTED&&error<0?(int)error:SF_BAD_FORMAT;}
typedef struct {
 const SfPersistVerifyTransport*transport;
 uint64_t nonce,deadline;
 unsigned sequence;
} VerifyRead;
static int verify_read(void*context,uint64_t lba,uint8_t out[512]){
 VerifyRead*r=context;const SfPersistVerifyTransport*t=r->transport;
 if(lba<SF_TXN_HEADER||lba>=SF_TXN_BANK+2*SF_TXN_BANK_SECTORS||r->sequence>=SF_PERSIST_VERIFY_READS)return SF_INVALID;
 uint64_t now=0;int e=t->now(t->context,&now);if(e)return known_error(e);
 if(now>=r->deadline)return SF_TIMEOUT;
 SfMessage m;memset(&m,0,sizeof(m));
 m.h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_PERSIST_CANDIDATE_VERIFY,0,r->nonce,
                  sizeof(m)-sizeof(SfIpcHeader),0,r->deadline};
 m.mode=1;m.offset=lba;m.length=512;m.handle=++r->sequence;
 int64_t sent=t->send(t->context,&m);if(sent)return known_error(sent);
 int64_t from=t->receive(t->context,&m,r->deadline);if(from<0)return known_error(from);
 if(m.h.cap_count==1)t->close(t->context,m.handle);
 if(from!=1||m.h.size!=sizeof(SfIpcHeader)||m.h.protocol_major!=1||m.h.protocol_minor||
    m.h.opcode!=OP_PERSIST_CANDIDATE_VERIFY||m.h.flags!=1||m.h.request_id!=r->nonce||
    m.h.cap_count||m.h.payload_bytes!=sizeof(m)-sizeof(SfIpcHeader)||m.h.deadline_ns!=r->deadline||
    m.mode!=1||m.offset!=lba||m.handle!=r->sequence||m.status<SF_VERSION_REJECTED||m.status>0||
    m.length!=(m.status?0u:512u))return SF_BAD_FORMAT;
 for(unsigned i=m.status?0u:512u;i<sizeof(m.data);i++)if(m.data[i])return SF_BAD_FORMAT;
 if(m.status)return(int)m.status;
 e=t->now(t->context,&now);if(e)return known_error(e);if(now>=r->deadline)return SF_TIMEOUT;
 memcpy(out,m.data,512);return 0;
}
int persist_verify_dispatch(SfPersistVerify*s,int64_t sender,const SfMessage*m,uint64_t now,int available,
                            SfPersistVolumeVerify verify,const SfPersistVerifyTransport*t,SfPersistSnapshot*out){
 if(!s||!m||!out||!verify||!t||!t->send||!t->receive||!t->now||!t->close)return SF_INVALID;
 if(sender!=1)return SF_DENIED;
 if(m->h.size!=sizeof(SfIpcHeader)||m->h.protocol_major!=1||m->h.protocol_minor||
    m->h.opcode!=OP_PERSIST_CANDIDATE_VERIFY||m->h.flags!=1||!m->h.request_id||m->h.cap_count||
    m->h.payload_bytes!=sizeof(*m)-sizeof(SfIpcHeader)||!m->h.deadline_ns||
    m->status||m->handle||m->offset||m->mode||m->length!=sizeof(SfPersistSnapshot))return SF_INVALID;
 for(unsigned i=sizeof(SfPersistSnapshot);i<sizeof(m->data);i++)if(m->data[i])return SF_INVALID;
 if(m->h.deadline_ns<=now)return SF_TIMEOUT;
 if(m->h.deadline_ns>verify_limit(now))return SF_INVALID;
 if(m->h.request_id<=s->last_nonce)return SF_STALE_HANDLE;
 s->last_nonce=m->h.request_id;
 if(available!=1)return SF_BUSY;
 if(s->attempted)return SF_DRAINING;
 s->attempted=1;
 SfPersistSnapshot expected;memcpy(&expected,m->data,sizeof(expected));
 if(expected.size!=sizeof(expected)||expected.version!=1||!expected.generation||expected.notes_bytes>SF_TXN_FILE_BYTES)return SF_INVALID;
 VerifyRead reader={t,m->h.request_id,m->h.deadline_ns,0};SfPersistSnapshot staged;
 memset(&staged,0,sizeof(staged));int e=verify(&expected,verify_read,&reader,&staged);
 if(e)return known_error(e);
 if(memcmp(&staged,&expected,sizeof(staged)))return SF_STALE_HANDLE;
 uint64_t after=0;e=t->now(t->context,&after);if(e)return known_error(e);if(after>=reader.deadline)return SF_TIMEOUT;
 *out=staged;return 0;
}
void persist_verify_reply(SfMessage*m,int status,uint64_t now,const SfPersistSnapshot*snapshot){
 if(!m)return;
 if(status<SF_VERSION_REJECTED||status>0||(!status&&!snapshot))status=SF_BAD_FORMAT;
 uint64_t nonce=m->h.request_id,deadline=m->h.deadline_ns;
 if(deadline<=now||deadline>verify_limit(now))deadline=verify_limit(now);
 memset(m,0,sizeof(*m));m->h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_PERSIST_CANDIDATE_VERIFY,0,nonce,
                                      sizeof(*m)-sizeof(SfIpcHeader),0,deadline};
 m->status=status;if(!status){m->length=sizeof(*snapshot);memcpy(m->data,snapshot,sizeof(*snapshot));}
}
