#include "libsf.h"
static long block_fault_send(uint32_t,SfMessage*);
#define sf_send block_fault_send
#include "../../services/virtio_blk.c"
#undef sf_send

/* Signed test driver only. Ordinary committed note content arms one READ
 * reply fault; the production driver has no arming or fault branch. */
static unsigned fault_armed,fault_done;
static int contains(const SfMessage*m,const char*text){
 size_t n=strlen(text);if(m->length>sizeof(m->data))return 0;
 for(size_t i=0;i+n<=m->length;i++)if(!memcmp(m->data+i,text,n))return 1;
 return 0;
}
static long block_fault_send(uint32_t ep,SfMessage*m){
 if(!m->status&&m->h.opcode==OP_BLOCK_WRITE){
  if(contains(m,"BFR-ARM-ROOT"))fault_armed=1;
  if(contains(m,"BFR-ARM-PERSIST"))fault_armed=2;
 }
 if(!fault_done&&fault_armed&&!m->status&&m->h.opcode==OP_BLOCK_READ&&m->length==256&&
    (fault_armed==1?m->h.flags==1:m->h.flags>1)){
  fault_done=1;sf_log(fault_armed==1?"[BLOCK-FAULT] Root READ wrong opcode\n":"[BLOCK-FAULT] Persist READ wrong opcode\n");
  unsigned opcode=m->h.opcode;m->h.opcode=OP_BLOCK_FLUSH;
  long r=sf_send(ep,m);m->h.opcode=opcode;return r;
 }
 return sf_send(ep,m);
}
