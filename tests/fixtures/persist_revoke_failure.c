#include "libsf.h"
static long revoke_fault_send(uint32_t,SfMessage*);
#define sf_send revoke_fault_send
#include "../../services/persist.c"
#undef sf_send
#ifndef SF_REVOKE_FAILURE_STATUS
#define SF_REVOKE_FAILURE_STATUS SF_DEVICE_GONE
#endif

/* A signed test service fails after a real successful policy commit. */
static int revoke_reported;
static long revoke_fault_send(uint32_t ep,SfMessage*m){
 if(!revoke_reported&&m->h.opcode==OP_PERSIST_REVOKE&&!m->status){
  revoke_reported=1;m->status=SF_REVOKE_FAILURE_STATUS;
  sf_log("[PERSIST-FAULT] revoke committed, reports failure code=");sf_number(-(int64_t)SF_REVOKE_FAILURE_STATUS);sf_log("\n");
 }
 return sf_send(ep,m);
}
