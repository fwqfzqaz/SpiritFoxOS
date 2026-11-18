/* Signed guest-only snapshot reply faults; cached volume and ordinary code
 * are production implementations. No fabricated volume or dirty latch. */
#include "libsf.h"
#include "../../services/persist_snapshot.h"
#ifndef SNAPSHOT_FAULT_MODE
#define SNAPSHOT_FAULT_MODE 1
#endif
#if SNAPSHOT_FAULT_MODE < 1 || SNAPSHOT_FAULT_MODE > 2
#error SNAPSHOT_FAULT_MODE must be1 (bad trust digest) or2 (lost ACK)
#endif
static long snapshot_fault_send(uint32_t,SfMessage*);
#define sf_send snapshot_fault_send
#include "../../services/persist.c"
#undef sf_send
static long snapshot_fault_send(uint32_t ep,SfMessage*m){
 if(m->h.opcode==OP_PERSIST_SNAPSHOT&&!m->status&&m->length==sizeof(SfPersistSnapshot)){
#if SNAPSHOT_FAULT_MODE == 1
  sf_log("[PERSIST-SNAPSHOT-FAULT] trust digest corrupted\n");
  m->data[__builtin_offsetof(SfPersistSnapshot,trust_sha256)]^=1;
#else
  sf_log("[PERSIST-SNAPSHOT-FAULT] reply dropped\n");return SF_TIMEOUT;
#endif
 }
 return sf_send(ep,m);
}
