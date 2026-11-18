/* Signed guest-only faults after the real provisional-cap dispatcher. */
#include "libsf.h"
#ifndef STAGE_FAULT_MODE
#define STAGE_FAULT_MODE 1
#endif
#if STAGE_FAULT_MODE < 1 || STAGE_FAULT_MODE > 2
#error STAGE_FAULT_MODE must be 1 (bad first ACK) or 2 (lost second ACK)
#endif
static long stage_fault_send(uint32_t,SfMessage*);
#define sf_send stage_fault_send
#include "../../services/persist.c"
#undef sf_send
static long stage_fault_send(uint32_t ep,SfMessage*m){
 if(m->h.opcode==OP_PERSIST_CANDIDATE_CAPS&&!m->status){
#if STAGE_FAULT_MODE == 1
  if(m->mode==1){sf_log("[PERSIST-STAGE-FAULT] first ACK malformed\n");m->length--;}
#else
  if(m->mode==2){sf_log("[PERSIST-STAGE-FAULT] second ACK dropped\n");return SF_TIMEOUT;}
#endif
 }
 return sf_send(ep,m);
}
