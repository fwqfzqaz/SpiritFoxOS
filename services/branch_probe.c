#include "libsf.h"
#include "arguments.h"

static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){sf_log("IPC BRANCH APP FAILED line=");sf_number(__LINE__);sf_log("\n");return 1;}}while(0)
static int ping(uint32_t ep,uint32_t pid,uint64_t id){
 SfMessage m;sf_message(&m,OP_ECHO,id);
 return !sf_send(ep,&m)&&sf_recv(ep,&m,sf_deadline(500))==(long)pid&&m.h.flags==pid&&m.h.request_id==id;
}
long sf_main(const SfBootArgs*a,size_t bytes){
 CHECK(!sf_boot_arguments_validate(a,bytes));SfMessage m;
 CHECK(sf_recv(a->service,&m,sf_deadline(3000))==1&&m.h.flags==1&&m.h.opcode==OP_ECHO&&
       m.h.request_id==0xb001&&m.h.cap_count==1&&m.handle);
 uint32_t shared=(uint32_t)m.handle;
 CHECK(sf_cap_revoke_branch(a->manager)==SF_DENIED);
 long middle=sf_cap_derive(shared,7),leaf=sf_cap_derive(middle,7),inbox=sf_ep_create();
 CHECK(middle>0&&leaf>0&&inbox>0);
 CHECK(ping(leaf,a->pid,0xb100));
 for(unsigned i=0;i<2;i++){sf_message(&m,OP_ECHO,0xb200+i);CHECK(!sf_send_cap(inbox,&m,leaf));}
 CHECK(sf_recv(inbox,&m,sf_deadline(500))==(long)a->pid&&m.h.cap_count==1&&m.handle);
 uint32_t installed=(uint32_t)m.handle;
 CHECK(ping(installed,a->pid,0xb300));
 CHECK(!sf_cap_close(middle)&&!sf_cap_close(leaf));
 sf_message(&m,OP_HELLO,0xb002);CHECK(!sf_send(a->manager,&m));
 CHECK(sf_recv(a->service,&m,sf_deadline(3000))==1&&m.h.flags==1&&m.h.opcode==OP_ECHO&&
       m.h.request_id==0xb003&&m.h.cap_count==1&&m.handle);
 uint32_t sibling=(uint32_t)m.handle;
 sf_message(&m,OP_ECHO,0xb400);
 CHECK(sf_send(shared,&m)==SF_STALE_HANDLE&&sf_send(installed,&m)==SF_STALE_HANDLE);
 CHECK(sf_recv(inbox,&m,sf_deadline(500))==(long)a->pid&&m.h.request_id==0xb201&&
       m.status==SF_REVOKED&&!m.h.cap_count&&!m.handle);
 CHECK(ping(sibling,a->pid,0xb500));
 CHECK(!sf_cap_close(sibling)&&!sf_cap_close(inbox));
 sf_log("IPC BRANCH APP PASSED checks=");sf_number(checks);sf_log("\n");return 0;
}
