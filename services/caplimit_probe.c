#include "libsf.h"
#include "arguments.h"
#include "sf_file.h"
static int gate(const SfBootArgs*a,uint64_t stage){
 SfMessage m;long from=sf_recv(a->service,&m,sf_deadline(3000));return from==1&&m.h.flags==1&&m.h.opcode==OP_ECHO&&m.h.request_id==stage&&!m.status?0:1;
}
static int acknowledge(const SfBootArgs*a,uint64_t stage){SfMessage m;sf_message(&m,OP_HELLO,stage);return sf_send(a->manager,&m);}
long sf_main(const SfBootArgs*a,size_t bytes){
 if(sf_boot_arguments_validate(a,bytes)||gate(a,1))return 1;
 if(sf_sys(SYS_TASK_CAP_LIMIT,a->pid,0,SF_LIMIT_QUERY)!=SF_DENIED||sf_sys(SYS_TASK_CAP_LIMIT,a->pid,32,SF_LIMIT_SET)!=SF_DENIED)return 2;
 long extra=sf_sys(SYS_EP_CREATE,0,0,0);if(extra<=0)return 3;
 for(unsigned i=0;i<64;i++)if(sf_sys(SYS_EP_CREATE,0,0,0)!=SF_NO_SPACE)return 4;
 if(sf_sys(SYS_CAP_DERIVE,extra,RIGHT_SEND,0)!=SF_NO_SPACE||acknowledge(a,1)||gate(a,2))return 5;
 if(sf_sys(SYS_CAP_CLOSE,extra,0,0)||acknowledge(a,2)||gate(a,3))return 6;
 for(unsigned i=0;i<64;i++)if(sf_sys(SYS_EP_CREATE,0,0,0)!=SF_NO_SPACE)return 7;
 SfFileClient files;if(sf_file_client_init(&files,a->manager,a->service,3000))return 8;
 int64_t root=sf_file_root(&files);if(root<=0)return 9;
 int64_t file=sf_file_openat(&files,root,"notes",SF_OPEN_READ);if(file<=0)return 10;
 uint8_t data[32];if(sf_file_read(&files,file,data,sizeof(data))<0||sf_file_close(&files,file)||sf_file_close(&files,root)||files.error)return 11;
 sf_log("CAP LIMIT APP PASSED\n");return 0;
}
