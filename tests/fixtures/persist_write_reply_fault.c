/* Signed QEMU fixture only. Production keeps services/persist.c unchanged. */
#include "libsf.h"
static long write_reply_fault_send(uint32_t,SfMessage*);
#define sf_send write_reply_fault_send
#include "../../services/persist.c"
#undef sf_send
#ifndef SF_WRITE_REPLY_EXCESS
#define SF_WRITE_REPLY_EXCESS 0
#endif
#if SF_WRITE_REPLY_EXCESS != 0 && SF_WRITE_REPLY_EXCESS != 1
#error SF_WRITE_REPLY_EXCESS must be zero or one
#endif

static unsigned write_reply_reported;
static long write_reply_fault_send(uint32_t ep,SfMessage*m){
 /* Log any later Persist RPC, including read-only queries: unchanged disk
  * bytes alone cannot prove that Root kept its uncertain-completion latch. */
 if(write_reply_reported&&m->h.opcode>=OP_PERSIST_ATTACH_SEND&&m->h.opcode<=OP_PERSIST_QUIESCENT){
  sf_log("[WRITE-ACK] post-fault request opcode=");sf_number(m->h.opcode);sf_log("\n");
 }
 static const char marker[]="PWR-ACK-COMMIT-";
 if(!write_reply_reported&&m->h.opcode==OP_PERSIST_WRITE&&
    m->length>=sizeof(marker)-1&&m->length<=sizeof(m->data)&&
    m->status==(int64_t)m->length&&!memcmp(m->data,marker,sizeof(marker)-1)){
  /* dispatch has returned the full length only after persist_notes_write's
   * actual transaction/flush succeeds. Change length once; retain the real
   * sender, request, opcode, status, offset, handle, mode and deadline. */
  uint32_t requested=m->length;write_reply_reported=1;
  m->length=SF_WRITE_REPLY_EXCESS?requested+1u:0u;
  sf_log("[WRITE-ACK] committed requested=");sf_number(requested);
  sf_log(" reply-length=");sf_number(m->length);sf_log(" status=");sf_number(m->status);sf_log("\n");
 }
 return sf_send(ep,m);
}
