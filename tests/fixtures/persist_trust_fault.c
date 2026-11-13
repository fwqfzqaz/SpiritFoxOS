/* Signed QEMU fixture only. Production bundles keep services/persist.c. */
#include "libsf.h"
static long trust_fault_send(uint32_t,SfMessage*);
#define sf_send trust_fault_send
#include "../../services/persist.c"
#undef sf_send
static unsigned query_fault,revoke_fault;
static long trust_fault_send(uint32_t ep,SfMessage*m){
 if(m->h.opcode==OP_PERSIST_TRUNCATE&&m->offset==12345&&!m->status)query_fault=1;
 if(m->h.opcode==OP_PERSIST_REVOKE&&m->status>=0)revoke_fault=1;
 if(m->h.opcode==OP_PERSIST_REVOKED&&!m->status){
  if(revoke_fault){revoke_fault=0;m->length=0;}
  else if(query_fault){query_fault=0;m->handle=0;m->length=32;sf_sha256(m->data,sf_trust_store.keys[1].public_key,32);}
 }
 return sf_send(ep,m);
}
