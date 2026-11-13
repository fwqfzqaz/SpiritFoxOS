#include "libsf.h"
/* Deliberately not an application child: exercise a rejected manager peer
 * while the owner of the delegated endpoint remains alive. */
long sf_main(const SfBootArgs*a,size_t size){
 if(size!=sizeof(*a)||a->abi_major!=1||!a->manager||!a->service)return 1;
 long cap=sf_ep_create();if(cap<=0)return 2;
 for(unsigned i=1;i<=65;i++){
  SfMessage m;long from=sf_recv(a->service,&m,sf_deadline(3000));
  if(from!=1||m.h.flags!=1||m.h.opcode!=OP_ECHO||m.h.request_id!=i||m.h.cap_count||m.status)return 3;
  if(i==65){if(m.mode!=1)return 4;break;}if(m.mode)return 5;
  sf_message(&m,OP_HELLO,i);if(sf_send_cap(a->manager,&m,(uint64_t)cap))return 6;
 }
 return sf_cap_close((uint64_t)cap)?7:0;
}
