/* Signed guest-only reply/exit faults. Ordinary provider code is unchanged. */
#include "libsf.h"
#ifndef IDLE_FAULT_MODE
#define IDLE_FAULT_MODE 1
#endif
#if IDLE_FAULT_MODE < 1 || IDLE_FAULT_MODE > 3
#error IDLE_FAULT_MODE must be 1 (bad probe ACK), 2 (lost discard ACK), 3 (late discard exit)
#endif
static long idle_fault_send(uint32_t,SfMessage*);
#define sf_send idle_fault_send
#include "../../services/persist.c"
#undef sf_send
static long idle_fault_send(uint32_t ep,SfMessage*m){
 if(m->h.opcode==OP_PERSIST_CANDIDATE_PROBE&&!m->status&&m->mode==1){
#if IDLE_FAULT_MODE == 1
  sf_log("[PERSIST-IDLE-FAULT] malformed probe ACK\n");m->mode=0;
#endif
 }
 if(m->h.opcode==OP_PERSIST_CANDIDATE_DISCARD&&!m->status&&m->mode==1){
#if IDLE_FAULT_MODE == 2
  sf_log("[PERSIST-IDLE-FAULT] discard ACK dropped\n");return SF_TIMEOUT;
#elif IDLE_FAULT_MODE == 3
  long r=sf_send(ep,m);
  if(!r){sf_log("[PERSIST-IDLE-FAULT] discard exit delayed\n");for(unsigned i=0;i<8;i++)sf_yield();}
  return r;
#endif
 }
 return sf_send(ep,m);
}
