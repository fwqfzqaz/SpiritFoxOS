/* Signed guest-only final attestation faults. All volume reconstruction and
 * broker reads use the real production implementation. */
#include "libsf.h"
#include "../../services/persist_verify.h"
#ifndef VERIFY_FAULT_MODE
#define VERIFY_FAULT_MODE 1
#endif
#if VERIFY_FAULT_MODE < 1 || VERIFY_FAULT_MODE > 2
#error VERIFY_FAULT_MODE must be1 (bad final snapshot) or2 (lost final reply)
#endif
static long verify_fault_send(uint32_t,SfMessage*);
#define sf_send verify_fault_send
#include "../../services/persist.c"
#undef sf_send
static long verify_fault_send(uint32_t ep,SfMessage*m){
 if(m->h.opcode==OP_PERSIST_CANDIDATE_VERIFY&&!m->mode&&!m->status&&m->length==sizeof(SfPersistSnapshot)){
#if VERIFY_FAULT_MODE == 1
  sf_log("[PERSIST-VERIFY-FAULT] final digest corrupted\n");m->data[__builtin_offsetof(SfPersistSnapshot,files_sha256)]^=1;
#else
  sf_log("[PERSIST-VERIFY-FAULT] final reply dropped\n");return SF_TIMEOUT;
#endif
 }
 return sf_send(ep,m);
}
