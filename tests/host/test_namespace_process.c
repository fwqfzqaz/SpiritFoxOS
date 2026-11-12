#include <stdio.h>
#include <stdlib.h>
#define LIBSF_H
#include "sf.h"
static long sf_send(uint32_t,SfMessage*);
static long sf_recv(uint32_t,SfMessage*,uint64_t);
static long sf_cap_close(uint64_t);
static uint64_t sf_deadline(unsigned ms){return 10000000ull+(uint64_t)ms*1000000;}
static void sf_message(SfMessage*m,unsigned op,uint64_t id){memset(m,0,sizeof(*m));m->h.size=40;m->h.protocol_major=1;m->h.opcode=op;m->h.request_id=id;m->h.payload_bytes=sizeof(*m)-40;m->h.deadline_ns=sf_deadline(3000);}
static void sf_log(const char*s){(void)s;}
#include "fs_main.c"
#include "namespace_client.h"
static SfNsClient client;
static unsigned checks,closed,requests,answers,live_checks,leases,reads;
static unsigned script;
static int live=1,bad_close,bad_sender;
static SfMessage callback,last_answer;
static uint8_t media[4096*512];
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"FS PROCESS FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static long sf_cap_close(uint64_t h){CHECK(h==0x777||h==0x101||h==0x102);closed++;return 0;}
static long sf_send(uint32_t h,SfMessage*m){
 if(h==32){last_answer=*m;answers++;return 0;}
 CHECK(h==0x101);callback=*m;return 0;
}
static long sf_recv(uint32_t h,SfMessage*m,uint64_t deadline){
 if(h==31){
  CHECK(!deadline);if(script==3)return SF_TIMEOUT;
  sf_message(m,script==2?OP_FS_ATTACH_REPLY:OP_FS_ATTACH_SEND,script+1);m->h.flags=script?1:2;m->h.cap_count=1;m->handle=script==0?0x777:script==1?0x101:0x102;
  script++;return script==1?2:1;
 }
 CHECK(h==0x102&&deadline==sf_deadline(30000));*m=callback;unsigned wanted=m->length;m->length=0;m->status=0;m->h.flags=1;m->h.deadline_ns=0;
 switch(m->h.opcode){
 case OP_FS_BLOCK_READ:CHECK(m->offset%512==0&&(m->mode==0||m->mode==256)&&wanted==256&&m->offset+512<=sizeof(media));memcpy(m->data,media+m->offset+m->mode,256);m->length=256;reads++;break;
 case OP_NS_CB_STORAGE_LIVE:live_checks++;if(!live)m->status=SF_DEVICE_GONE;break;
 case OP_NS_CB_MOUNT_OPEN:CHECK(m->handle==1);leases++;break;
 case OP_NS_CB_MOUNT_CLOSE:CHECK(m->handle==1&&leases);if(bad_close)m->status=SF_BUSY;else leases--;break;
 case OP_NS_CB_CONSOLE:m->status=wanted;break;
 default:m->status=SF_INVALID;break;
 }return bad_sender?2:1;
}
static int64_t request(SfMessage*m){m->h.flags=1;requests++;return process_request(1,m);}
static int64_t exchange_root(void*c,SfMessage*m){(void)c;request(m);if(sf_nsv_faulted())return SF_DEVICE_GONE;m->h.flags=77;return 77;}
int main(void){
 CHECK(sf_main(0,0)==SF_ABI_MISMATCH);SfBootArgs a={0};a.abi_major=1;a.service=31;a.manager=32;
 CHECK(sf_main(&a,sizeof(a))==SF_TIMEOUT&&script==3&&answers==3&&closed==1);
 CHECK(broker_ep==0x101&&broker_reply==0x102&&!capacity&&!sf_nsv_ready()&&last_answer.status==0);
 CHECK(!sf_nsc_connect(&client,77,exchange_root,0,0,SF_NS_B_ALL));
 CHECK(!sf_nsc_create(&client,"/ram",NODE_FILE,0,0,0,0644));int ram=sf_nsc_open(&client,42,"/","/ram",OPEN_READ|OPEN_WRITE,0,0);CHECK(ram>0);
 CHECK(sf_nsc_write(&client,42,(uint32_t)ram,"kept",4)==4);CHECK(sf_nsc_seek(&client,42,(uint32_t)ram,0)==0);
 char text[96]={0};CHECK(sf_nsc_read(&client,42,(uint32_t)ram,text,4,0,0)==4&&!memcmp(text,"kept",4)&&!reads);
 FILE*f=fopen("build/persistent.img","rb");CHECK(f&&fread(media,1,sizeof(media),f)==sizeof(media));fclose(f);
 SfMessage m;sf_message(&m,OP_FS_ATTACH_SEND,20);m.h.cap_count=1;m.handle=0x101;m.offset=sizeof(media);CHECK(!request(&m));
 sf_message(&m,OP_FS_MOUNT,21);m.offset=1024*1024;CHECK(!request(&m)&&fs.mounted&&m.handle==fs.count&&fs.count>0&&reads>0);
 int disk=sf_nsc_open(&client,42,"/","/disk/etc/readme.txt",OPEN_READ,0,0);CHECK(disk>0&&leases==1);
 CHECK(sf_nsc_read(&client,42,(uint32_t)disk,text,sizeof(text),0,0)>0&&live_checks==1);
 unsigned io=reads;live=0;text[0]='x';CHECK(sf_nsc_read(&client,42,(uint32_t)disk,text,1,UINT64_MAX,1)==SF_DEVICE_GONE&&text[0]=='x'&&reads==io&&live_checks==2);live=1;
 CHECK(sf_nsc_unmount(&client,1)==SF_BUSY);CHECK(!sf_nsc_close(&client,42,(uint32_t)disk)&&!leases);CHECK(!sf_nsc_unmount(&client,1));
 sf_message(&m,OP_FS_STOP,22);CHECK(!request(&m)&&!fs.mounted);
 CHECK(sf_nsc_seek(&client,42,(uint32_t)ram,0)==0&&sf_nsc_read(&client,42,(uint32_t)ram,text,4,0,0)==4&&!memcmp(text,"kept",4));
 /* Both Root identity fields matter, and rejected capability transfers close. */
 sf_message(&m,OP_NS_INIT,23);m.h.flags=2;CHECK(process_request(1,&m)==SF_INVALID);
 sf_message(&m,OP_NS_INIT,24);m.h.flags=1;m.h.cap_count=1;m.handle=0x777;io=closed;CHECK(process_request(2,&m)==SF_DENIED&&closed==io+1&&!m.h.cap_count);
 /* Legacy disk consumers without NS_INIT still mount/read; zero-length find
  * is rejected before touching an unterminated or arbitrary path. */
 sf_nsv_init(&fs,exchange,0);sf_message(&m,OP_FS_MOUNT,25);CHECK(!request(&m));
 sf_message(&m,OP_FS_FIND,26);CHECK(request(&m)==SF_INVALID);
 sf_message(&m,OP_FS_READ,27);m.mode=0;m.length=1;CHECK(!request(&m)&&m.length==1);
 fs.mounted=0;sf_nsv_init(&fs,exchange,0);CHECK(!sf_nsc_connect(&client,77,exchange_root,0,0,SF_NS_B_ALL));
 sf_message(&m,OP_FS_MOUNT,28);m.offset=1024*1024;CHECK(!request(&m));disk=sf_nsc_open(&client,42,"/","/disk/etc/readme.txt",OPEN_READ,0,0);CHECK(disk>0);
 bad_close=1;CHECK(sf_nsc_close(&client,42,(uint32_t)disk)==SF_DEVICE_GONE&&sf_nsv_faulted()&&!client.ready);
 printf("Namespace Fs-process tests passed: %u assertions; first-sender pin, capability cleanup, driverless RAM, real SFRFS broker, live EOF and disk-independent RAM; host exchange model\n",checks);return 0;
}
