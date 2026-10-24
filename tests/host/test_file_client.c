#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SF_FILE_SYSCALL model_syscall
#include "../../services/file_client.c"
#include "file_rpc.h"
static unsigned checks,calls,sends,receives,corrupt,stales;
static int64_t tick_value=10,send_error,recv_error,server_error;
static SfMessage sent;static uint64_t saved_deadline,last_sent;
static SfNamespace real_namespace;static uint32_t real_root;static int use_server;static SfMessage real_response;static unsigned console_bytes;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FILE CLIENT FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void reset(void){calls=sends=receives=corrupt=stales=0;tick_value=10;send_error=recv_error=server_error=0;memset(&sent,0,sizeof(sent));saved_deadline=last_sent=0;use_server=0;}
static long console(const void*data,size_t bytes){CHECK(data&&bytes<=256);console_bytes+=bytes;return bytes;}
static SfFileClient client(void){SfFileClient c;CHECK(!sf_file_client_init(&c,0x101,0x102,3000));return c;}
int64_t model_syscall(uint64_t op,uint64_t a,uint64_t b,uint64_t deadline){
 calls++;if(op==SYS_TICKS)return tick_value;
 SfMessage*m=(void*)(uintptr_t)b;
 if(op==SYS_SEND){
  sends++;CHECK(a==0x101&&!deadline);CHECK(m->h.size==40&&m->h.protocol_major==1&&!m->h.protocol_minor&&!m->h.cap_count&&!m->h.flags&&m->h.payload_bytes==sizeof(*m)-40);
  CHECK(m->h.request_id>last_sent&&m->length<=256);last_sent=m->h.request_id;sent=*m;saved_deadline=m->h.deadline_ns;
  if(use_server&&!send_error){real_response=*m;sf_file_dispatch(&real_namespace,42,&real_root,&real_response);real_response.h.flags=1;}
  return send_error;
 }
 CHECK(op==SYS_RECV&&a==0x102);receives++;CHECK(deadline==saved_deadline);if(tick_value<INT64_MAX)tick_value++;if(recv_error)return recv_error;
 if(use_server){*m=real_response;return 1;}
 *m=sent;m->h.flags=1;m->length=0;m->status=0;
 switch(m->h.opcode){
 case OP_ROOT:case OP_OPEN:case OP_OPENAT:m->handle=0x201;break;
 case OP_READ:case OP_ENUMERATE:m->status=m->length=sent.length;for(unsigned i=0;i<m->length;i++)m->data[i]=(uint8_t)i;break;
 case OP_WRITE:m->status=sent.length;m->length=sent.length;break;
 case OP_STAT:{SfFileStat s={32,2,1,4,37,0};m->length=32;memcpy(m->data,&s,32);break;}
 case OP_SEEK:m->status=sent.offset;break;
 case OP_POLL:m->status=1;break;
 }
 if(server_error){m->status=server_error;memset(m->data,0x5a,sizeof(m->data));}
 if(stales){stales--;m->h.request_id--;m->h.opcode=OP_CLOSE;return 1;}
 switch(corrupt){
 case 1:return 2;case 2:m->h.flags=2;break;case 3:m->h.size=39;break;case 4:m->h.protocol_major=2;break;case 5:m->h.protocol_minor=1;break;
 case 6:m->h.payload_bytes--;break;case 7:m->h.cap_count=1;break;case 8:m->length=257;break;case 9:m->h.request_id=0;break;case 10:m->h.request_id++;break;case 11:m->h.opcode++;break;
 case 12:m->handle|=1ull<<32;break;case 13:m->handle=0;break;case 14:m->status=1;break;
 case 15:m->status=(int64_t)sent.length+1;break;case 16:m->length--;break;case 17:m->status=(int64_t)sent.offset+1;break;case 18:m->status=8;break;
 case 19:m->length=31;break;case 20:m->data[0]=31;break;case 21:m->data[4]=6;break;case 22:m->data[8]=0;break;case 23:m->data[8]=16;break;case 24:m->data[12]=8;break;
 }
 return 1;
}
int main(void){
 SfFileClient c,original;memset(&c,0xa5,sizeof(c));original=c;
 CHECK(sf_file_client_init(0,1,2,3000)==SF_INVALID);
 uint64_t invalid[]={0,1ull<<32,1ull<<63,UINT64_MAX};for(unsigned i=0;i<4;i++){CHECK(sf_file_client_init(&c,invalid[i],2,3000)==SF_INVALID);CHECK(!memcmp(&c,&original,sizeof(c)));CHECK(sf_file_client_init(&c,1,invalid[i],3000)==SF_INVALID);}
 CHECK(sf_file_client_init(&c,1,2,0)==SF_INVALID);CHECK(sf_file_client_init(&c,1,2,60001)==SF_INVALID);
 uint8_t buffer[320],copy[320];SfFileStat info,info_copy;
 for(unsigned round=0;round<1000;round++){
  reset();c=client();CHECK(sf_file_root(&c)==0x201);CHECK(sf_file_console(&c)==0x201);CHECK(sent.h.opcode==OP_OPEN&&sent.mode==SF_OPEN_WRITE&&sent.length==13&&!strcmp((char*)sent.data,"/dev/console"));
  CHECK(sf_file_openat(&c,0x201,"notes",SF_OPEN_READ)==0x201);CHECK(sent.handle==0x201&&sent.mode==1&&sent.length==6&&!strcmp((char*)sent.data,"notes"));
  memset(buffer,0xa5,sizeof(buffer));CHECK(sf_file_read(&c,0x201,buffer,sizeof(buffer))==256);for(unsigned i=0;i<256;i++)CHECK(buffer[i]==(uint8_t)i);for(unsigned i=256;i<sizeof(buffer);i++)CHECK(buffer[i]==0xa5);
  CHECK(sf_file_read(&c,0x201,0,0)==0);CHECK(sf_file_write(&c,0x201,buffer,sizeof(buffer))==256);CHECK(sent.length==256&&!memcmp(sent.data,buffer,256));CHECK(sf_file_write(&c,0x201,0,0)==0);
  CHECK(sf_file_enumerate(&c,0x201,(char*)buffer,5)==5);CHECK(!sf_file_stat(&c,0x201,&info));CHECK(info.bytes==32&&info.type==SF_TYPE_FILE&&info.size==37);
  CHECK(sf_file_seek(&c,0x201,0x100000000ull)==0x100000000ll);CHECK(sf_file_poll(&c,0x201)==1);CHECK(!sf_file_truncate(&c,0x201,0x100000000ull)&&sent.offset==0x100000000ull);
  CHECK(!sf_file_unlink(&c,0x201,"old")&&sent.h.opcode==OP_UNLINK);CHECK(!sf_file_rmdir(&c,0x201,"empty")&&sent.h.opcode==OP_RMDIR);CHECK(!sf_file_close(&c,0x201));CHECK(!c.error);
 }
 reset();c=client();unsigned before=calls;uint64_t high=(1ull<<32)|0x201;
 CHECK(sf_file_read(&c,high,buffer,1)==SF_STALE_HANDLE);CHECK(sf_file_close(&c,0)==SF_STALE_HANDLE);CHECK(sf_file_stat(&c,high,&info)==SF_STALE_HANDLE);CHECK(sf_file_openat(&c,high,"notes",1)==SF_STALE_HANDLE);CHECK(!calls&&!c.error);
 CHECK(sf_file_read(&c,0x201,0,1)==SF_INVALID);CHECK(sf_file_write(&c,0x201,0,1)==SF_INVALID);CHECK(sf_file_stat(&c,0x201,0)==SF_INVALID);CHECK(sf_file_seek(&c,0x201,UINT64_MAX)==SF_INVALID);CHECK(sf_file_openat(&c,0x201,0,1)==SF_INVALID);CHECK(sf_file_unlink(&c,0x201,0)==SF_INVALID);CHECK(sf_file_rmdir(&c,0x201,"")==SF_INVALID);
 char path[257];memset(path,'a',256);path[256]=0;CHECK(sf_file_openat(&c,0x201,path,1)==SF_INVALID);CHECK(calls==before);
 reset();c=client();memset(buffer,0xa5,sizeof(buffer));memcpy(copy,buffer,sizeof(copy));server_error=SF_DENIED;CHECK(sf_file_read(&c,0x201,buffer,8)==SF_DENIED);CHECK(!memcmp(buffer,copy,sizeof(buffer))&&!c.error);server_error=0;CHECK(sf_file_root(&c)==0x201);
 for(unsigned mode=1;mode<=14;mode++){
  reset();c=client();corrupt=mode;CHECK(sf_file_root(&c)==SF_BAD_FORMAT);before=calls;CHECK(sf_file_root(&c)==SF_BAD_FORMAT&&calls==before);
 }
 for(unsigned mode=15;mode<=16;mode++){reset();c=client();corrupt=mode;memset(buffer,0xa5,sizeof(buffer));memcpy(copy,buffer,sizeof(copy));CHECK(sf_file_read(&c,0x201,buffer,8)==SF_BAD_FORMAT);CHECK(!memcmp(buffer,copy,sizeof(buffer)));}
 reset();c=client();corrupt=15;CHECK(sf_file_write(&c,0x201,"123",3)==SF_BAD_FORMAT);
 reset();c=client();corrupt=17;CHECK(sf_file_seek(&c,0x201,7)==SF_BAD_FORMAT);reset();c=client();corrupt=18;CHECK(sf_file_poll(&c,0x201)==SF_BAD_FORMAT);
 for(unsigned mode=19;mode<=24;mode++){reset();c=client();corrupt=mode;memset(&info,0xa5,sizeof(info));info_copy=info;CHECK(sf_file_stat(&c,0x201,&info)==SF_BAD_FORMAT);CHECK(!memcmp(&info,&info_copy,sizeof(info)));}
 reset();c=client();corrupt=14;CHECK(sf_file_close(&c,0x201)==SF_BAD_FORMAT);
 reset();c=client();recv_error=SF_TIMEOUT;CHECK(sf_file_read(&c,0x201,buffer,1)==SF_TIMEOUT);before=calls;recv_error=0;CHECK(sf_file_root(&c)==SF_TIMEOUT&&calls==before);
 uint64_t timed_out=sent.h.request_id;CHECK(!sf_file_client_init(&c,0x101,0x102,3000));stales=2;CHECK(sf_file_root(&c)==0x201&&sent.h.request_id>timed_out&&receives==4&&!stales);
 reset();c=client();stales=9;CHECK(sf_file_root(&c)==SF_BAD_FORMAT&&receives==9);reset();c=client();send_error=SF_WOULD_BLOCK;CHECK(sf_file_root(&c)==SF_WOULD_BLOCK&&!receives&&!c.error);send_error=0;CHECK(sf_file_root(&c)==0x201);
 reset();c=client();send_error=1;CHECK(sf_file_read(&c,0x201,buffer,1)==SF_BAD_FORMAT&&c.error==SF_BAD_FORMAT&&!receives);
 reset();c=client();tick_value=INT64_MAX;before=sf_file_sequence;CHECK(sf_file_root(&c)==SF_INVALID&&!sends&&!receives&&sf_file_sequence==before);
 /* Client codec against the real current dispatcher and namespace. Only the
  * syscall/manager routing is modeled; authority and file logic are linked. */
 reset();c=client();use_server=1;sf_ns_init(&real_namespace,console);CHECK(!sf_ns_create(&real_namespace,"/cap",NODE_DIR,0));CHECK(!sf_ns_create(&real_namespace,"/cap/notes",NODE_FILE,0));CHECK(!sf_ns_create(&real_namespace,"/dev",NODE_DIR,0));CHECK(!sf_ns_create(&real_namespace,"/dev/console",NODE_CONSOLE,0));
 int writer=sf_ns_open(&real_namespace,1,"/","/cap/notes",OPEN_WRITE);CHECK(writer>0&&sf_ns_write(&real_namespace,1,writer,"hello",5)==5&&!sf_ns_close(&real_namespace,1,writer));
 real_root=sf_ns_open(&real_namespace,42,"/","/cap",OPEN_READ|OPEN_DIRECTORY);CHECK(real_root>0&&sf_file_root(&c)==real_root);uint32_t root=real_root;
 CHECK(!sf_file_stat(&c,root,&info)&&info.type==SF_TYPE_DIRECTORY&&info.mode==(SF_OPEN_READ|SF_OPEN_DIRECTORY));
 int64_t file=sf_file_openat(&c,root,"notes",SF_OPEN_READ);CHECK(file>0);CHECK(sf_file_read(&c,file,buffer,sizeof(buffer))==5&&!memcmp(buffer,"hello",5));CHECK(!sf_file_stat(&c,file,&info)&&info.offset==5&&info.size==5);
 CHECK(sf_file_seek(&c,file,2)==2);CHECK(sf_file_read(&c,file,buffer,10)==3&&!memcmp(buffer,"llo",3));CHECK(sf_file_write(&c,file,0,0)==SF_DENIED);CHECK(sf_file_truncate(&c,file,0)==SF_DENIED);CHECK(sf_file_unlink(&c,root,"notes")==SF_DENIED);CHECK(sf_file_rmdir(&c,root,"empty")==SF_DENIED);CHECK(sf_file_openat(&c,root,"notes",SF_OPEN_WRITE)==SF_DENIED);
 CHECK(sf_file_enumerate(&c,root,(char*)buffer,sizeof(buffer))==6&&!memcmp(buffer,"notes\n",6));int64_t output=sf_file_console(&c);CHECK(output>0);console_bytes=0;CHECK(sf_file_write(&c,output,buffer,sizeof(buffer))==256&&console_bytes==256);CHECK(!sf_file_close(&c,output));
 CHECK(!sf_file_close(&c,root)&&!real_root);CHECK(sf_file_root(&c)==SF_DENIED);CHECK(!sf_file_stat(&c,file,&info)&&info.offset==5);CHECK(!sf_file_close(&c,file));memset(&info,0xa5,sizeof(info));info_copy=info;CHECK(sf_file_stat(&c,file,&info)==SF_STALE_HANDLE&&!memcmp(&info,&info_copy,sizeof(info)));
 real_root=sf_ns_open(&real_namespace,42,"/","/cap",OPEN_READ|OPEN_WRITE|OPEN_DIRECTORY);CHECK(real_root>0);CHECK(!sf_ns_create(&real_namespace,"/cap/empty",NODE_DIR,0));CHECK(!sf_file_rmdir(&c,real_root,"empty"));CHECK(sf_ns_open(&real_namespace,42,"/","/cap/empty",OPEN_READ)==SF_NOT_FOUND);CHECK(!sf_file_unlink(&c,real_root,"notes"));CHECK(sf_file_openat(&c,real_root,"notes",SF_OPEN_READ)==SF_NOT_FOUND);CHECK(!sf_file_close(&c,real_root));sf_ns_reap(&real_namespace,42);for(unsigned i=0;i<32;i++)CHECK(!real_namespace.descriptions[i].refs);
 reset();c=client();sf_file_sequence=UINT64_MAX-1;CHECK(sf_file_root(&c)==0x201&&sent.h.request_id==UINT64_MAX);before=calls;CHECK(sf_file_root(&c)==SF_NO_SPACE&&calls==before);CHECK(!sf_file_client_init(&c,0x101,0x102,3000));CHECK(sf_file_root(&c)==SF_NO_SPACE&&calls==before);
 printf("FILE CLIENT PASS: %u assertions; actual codec over modeled INT80 transport\n",checks);return 0;
}
