/* Signed test provider: corrupt or suppress only a completed inquiry reply. */
#include "libsf.h"
#if DRIVER_INFO_FAULT_MODE < 1 || DRIVER_INFO_FAULT_MODE > 2
#error DRIVER_INFO_FAULT_MODE must be 1 (malformed reply) or 2 (lost reply)
#endif
static long driver_info_fault_send(uint32_t,SfMessage*);
#define sf_send driver_info_fault_send
#include "../../services/persist.c"
#undef sf_send
static long driver_info_fault_send(uint32_t ep,SfMessage*message){
 if(message->h.opcode==OP_PERSIST_CANDIDATE_DRIVER_INFO&&!message->status){
#if DRIVER_INFO_FAULT_MODE == 1
  sf_log("[PERSIST-INFO-FAULT] malformed candidate reply\n");
  message->offset=0;
#else
  sf_log("[PERSIST-INFO-FAULT] suppressed candidate reply\n");
  return SF_TIMEOUT;
#endif
 }
 return sf_send(ep,message);
}
