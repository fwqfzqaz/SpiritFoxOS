#include "libsf.h"
#include "arguments.h"

static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){sf_log("IPC REVOCATION APP FAILED line=");sf_number(__LINE__);sf_log("\n");return 1;}}while(0)
static int ping(uint32_t ep,uint32_t pid,uint64_t id){
 SfMessage m;sf_message(&m,OP_ECHO,id);
 if(sf_send(ep,&m))return 0;
 return sf_recv(ep,&m,sf_deadline(500))==(long)pid&&m.h.flags==pid&&m.h.request_id==id;
}
long sf_main(const SfBootArgs*a,size_t bytes){
 CHECK(!sf_boot_arguments_validate(a,bytes));SfMessage m;
 CHECK(sf_recv(a->service,&m,sf_deadline(3000))==1&&m.h.flags==1&&m.h.opcode==OP_ECHO&&
       m.h.request_id==0x7265766full&&m.h.cap_count==1&&m.handle);
 uint32_t shared=(uint32_t)m.handle;
 long inbox=sf_ep_create();CHECK(inbox>0);
 for(unsigned round=0;round<8;round++){
  long parent=sf_cap_derive(shared,7),middle=sf_cap_derive(parent,7),leaf=sf_cap_derive(middle,7);
  CHECK(parent>0&&middle>0&&leaf>0);
  /* Move the queue head before an attached transfer crosses its ring end. */
  for(unsigned i=0;i<5;i++){sf_message(&m,OP_ECHO,i);CHECK(!sf_send(inbox,&m));}
  for(unsigned i=0;i<5;i++)CHECK(sf_recv(inbox,&m,sf_deadline(500))==(long)a->pid&&m.h.request_id==i);
  sf_message(&m,OP_ECHO,0x100+round);CHECK(!sf_send_cap(inbox,&m,leaf));
  if(round&1){CHECK(!sf_cap_close(middle));CHECK(!sf_cap_close(leaf));}
  CHECK(!sf_cap_revoke(parent));
  CHECK(sf_recv(inbox,&m,sf_deadline(500))==(long)a->pid&&m.h.request_id==0x100+round&&
        m.status==SF_REVOKED&&!m.h.cap_count&&!m.handle);
  CHECK(ping(shared,a->pid,0x200+round));
 }
 for(unsigned round=0;round<8;round++){
  long parent=sf_cap_derive(shared,7),middle=sf_cap_derive(parent,7),leaf=sf_cap_derive(middle,7);
  CHECK(parent>0&&middle>0&&leaf>0);
  sf_message(&m,OP_ECHO,0x300+round);CHECK(!sf_send_cap(inbox,&m,middle));
  CHECK(!sf_cap_close(middle));
  long reused=sf_cap_derive(shared,7);CHECK(reused>0&&(reused&255)==(middle&255)&&reused!=middle);
  CHECK(sf_recv(inbox,&m,sf_deadline(500))==(long)a->pid&&m.h.cap_count==1&&m.handle&&!m.status);
  uint32_t received=(uint32_t)m.handle;
  CHECK(ping(received,a->pid,0x400+round)&&ping(leaf,a->pid,0x500+round));
  CHECK(!sf_cap_revoke(parent));sf_message(&m,OP_ECHO,0x600+round);
  CHECK(sf_send(received,&m)==SF_STALE_HANDLE&&sf_send(leaf,&m)==SF_STALE_HANDLE);
  CHECK(ping(reused,a->pid,0x700+round)&&!sf_cap_close(reused));
 }
 CHECK(!sf_cap_close(inbox)&&!sf_cap_close(shared));
 sf_log("IPC REVOCATION APP PASSED checks=");sf_number(checks);sf_log("\n");return 0;
}
