#include "libsf.h"
#include "arguments.h"
static int put(uint32_t ep,uint64_t id,unsigned tag){SfMessage m;sf_message(&m,OP_ECHO,id);m.status=tag;return sf_send(ep,&m);}
static int take(uint32_t ep,uint32_t pid,uint64_t id,unsigned tag){SfMessage m;long from=sf_recv(ep,&m,sf_deadline(3000));return from==(long)pid&&m.h.flags==pid&&m.h.request_id==id&&m.status==(int64_t)tag?0:1;}
long sf_main(const SfBootArgs*a,size_t bytes){
 if(sf_boot_arguments_validate(a,bytes))return 1;SfMessage gate;
 if(sf_recv(a->service,&gate,sf_deadline(3000))!=1||gate.h.flags!=1||gate.h.opcode!=OP_ECHO||gate.h.request_id!=0xcace)return 2;
 long ep=sf_sys(SYS_EP_CREATE,0,0,0);if(ep<=0)return 3;const uint64_t high=0x100000007ull;
 if(put(ep,7,1)||put(ep,high,2)||put(ep,7,3)||put(ep,0,4)||sf_send_cancel(ep,7)!=2)return 4;
 if(take(ep,a->pid,high,2)||take(ep,a->pid,0,4)||sf_send_cancel(ep,7)||sf_send_cancel(ep,high))return 5;
 for(unsigned i=0;i<8;i++)if(put(ep,100+i,i))return 6;
 if(sf_send_cancel(ep,103)!=1)return 7;
 for(unsigned i=0;i<8;i++)if(i!=3&&take(ep,a->pid,100+i,i))return 8;
 for(unsigned i=0;i<8;i++)if(put(ep,0,i))return 9;
 if(sf_send_cancel(ep,0)!=8||sf_send_cancel(ep,0))return 10;
 SfMessage out;if(sf_recv(ep,&out,sf_deadline(10))!=SF_TIMEOUT||sf_sys(SYS_CAP_CLOSE,ep,0,0))return 11;
 /* The manager deliberately holds its own same-ID message while we withdraw
  * only this task's send through the inherited SEND-only capability. */
 SfMessage m;sf_message(&m,OP_HELLO,high);m.h.flags=1;m.status=77;
 if(sf_send(a->manager,&m)||sf_send_cancel(a->manager,high)!=1||sf_send_cancel(a->manager,high))return 12;
 sf_log("IPC CANCEL APP PASSED\n");return 0;
}
