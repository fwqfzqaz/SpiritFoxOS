#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SF_FILE_SYSCALL pread_syscall
#include "../../services/file_client.c"
#include "file_rpc.h"
static SfNamespace ns,saved;
static uint32_t actor=42,root;
static SfMessage reply;
static unsigned checks,sends,corrupt,backend_calls,backend_mode;
static uint64_t backend_offset;
static uint8_t expected[512];
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FILE PREAD FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static SfFileClient client(void){SfFileClient c;CHECK(!sf_file_client_init(&c,0x101,0x102,1000));return c;}
static long disk_read(unsigned index,uint64_t offset,void*out,size_t bytes){
 CHECK(index==7);backend_calls++;backend_offset=offset;
 if(backend_mode==1)return SF_DEVICE_GONE;
 if(backend_mode==2)return (long)bytes+1;
 if(backend_mode==3)return 1;
 if(offset>=sizeof(expected))return 0;
 if(bytes>sizeof(expected)-offset)bytes=sizeof(expected)-offset;
 if(backend_mode==4&&bytes>2)bytes=2;
 memcpy(out,expected+offset,bytes);return (long)bytes;
}
static void setup(void){
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/data",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/data/item",NODE_FILE,0));
 CHECK(!sf_ns_create(&ns,"/data/pipe",NODE_PIPE,0));CHECK(!sf_ns_create(&ns,"/data/console",NODE_CONSOLE,0));
 CHECK(!sf_ns_create(&ns,"/data/disk",NODE_DISK,1));
 for(unsigned i=0;i<sizeof(expected);i++)expected[i]=(uint8_t)((i*17+3)%251);
 int h=sf_ns_open(&ns,42,"/","/data/item",OPEN_WRITE);CHECK(h>0&&sf_ns_write(&ns,42,h,expected,sizeof(expected))==sizeof(expected));CHECK(!sf_ns_close(&ns,42,h));
 for(unsigned i=0;i<32;i++)if(!strcmp(ns.nodes[i].path,"/data/disk")){ns.nodes[i].external=7;ns.nodes[i].size=sizeof(expected);}
 ns.external_read=disk_read;root=sf_ns_open(&ns,42,"/","/data",OPEN_READ|OPEN_DIRECTORY);CHECK((int)root>0);
 actor=42;corrupt=backend_calls=backend_mode=0;
}
int64_t pread_syscall(uint64_t op,uint64_t a,uint64_t b,uint64_t deadline){
 if(op==SYS_TICKS)return 10;
 SfMessage*m=(void*)(uintptr_t)b;
 if(op==SYS_SEND){CHECK(a==0x101&&!deadline);sends++;reply=*m;sf_file_dispatch(&ns,actor,&root,&reply);reply.h.flags=1;return 0;}
 CHECK(op==SYS_RECV&&a==0x102&&deadline==reply.h.deadline_ns);*m=reply;
 if(corrupt&&m->h.opcode==OP_PREAD)switch(corrupt){
  case 1:m->length++;break;case 2:m->status=257;break;case 3:m->handle^=256;break;
  case 4:m->offset++;break;case 5:m->mode=1;break;case 6:m->h.opcode=OP_READ;break;
 }
 return 1;
}
static void raw(uint64_t handle,unsigned length,unsigned mode,uint64_t offset,long expected_status){
 struct{SfMessage m;uint8_t guard[16];} packet;memset(&packet,0xa5,sizeof(packet));
 packet.m.h.opcode=OP_PREAD;packet.m.handle=handle;packet.m.length=length;packet.m.mode=mode;packet.m.offset=offset;
 saved=ns;CHECK(sf_file_dispatch(&ns,actor,&root,&packet.m)==expected_status&&packet.m.status==expected_status);
 CHECK(!memcmp(&saved,&ns,sizeof(ns)));for(unsigned i=0;i<sizeof(packet.guard);i++)CHECK(packet.guard[i]==0xa5);
}
int main(void){
 setup();SfFileClient c=client();int file=sf_file_openat(&c,root,"item",OPEN_READ),alias=sf_file_dup(&c,file);CHECK(file>0&&alias>0);
 CHECK(sf_file_seek(&c,file,37)==37);uint8_t out[258];memset(out,0xa5,sizeof(out));saved=ns;
 CHECK(sf_file_pread(&c,alias,out+1,8,100)==8&&!memcmp(out+1,expected+100,8));CHECK(out[0]==0xa5&&out[9]==0xa5&&!memcmp(&saved,&ns,sizeof(ns)));
 SfFileStat info;CHECK(!sf_file_stat(&c,file,&info)&&info.offset==37);CHECK(sf_file_read(&c,file,out,1)==1&&out[0]==expected[37]);
 CHECK(!sf_file_close(&c,file));CHECK(sf_file_pread(&c,alias,out,1,0)==1&&out[0]==expected[0]);
 CHECK(!sf_file_stat(&c,alias,&info)&&info.offset==38);
 /* Independent byte-vector oracle, with unrelated shared cursor positions. */
 uint32_t seed=0x4a8f27u;
 for(unsigned i=0;i<1000;i++){
  seed=seed*1664525u+1013904223u;unsigned cursor=seed%1025;CHECK(sf_file_seek(&c,alias,cursor)==cursor);
  uint64_t offset=i%7==0?UINT64_MAX:i%7==1?UINT32_MAX+1ull:(seed>>7)%700;
  size_t requested=(seed>>17)%400,n=requested>256?256:requested;
  if(offset>=sizeof(expected))n=0;else if(n>sizeof(expected)-offset)n=sizeof(expected)-offset;
  memset(out,0xa5,sizeof(out));saved=ns;CHECK(sf_file_pread(&c,alias,out+1,requested,offset)==(int64_t)n);
  CHECK(!memcmp(&saved,&ns,sizeof(ns))&&out[0]==0xa5);
  if(n)CHECK(!memcmp(out+1,expected+offset,n));
  for(size_t j=n+1;j<sizeof(out);j++)CHECK(out[j]==0xa5);
 }
 saved=ns;CHECK(sf_file_pread(&c,alias,0,0,UINT64_MAX)==0&&!memcmp(&saved,&ns,sizeof(ns)));
 unsigned before=sends;CHECK(sf_file_pread(&c,alias,0,1,0)==SF_INVALID&&sends==before);
 CHECK(sf_file_pread(&c,0,out,1,0)==SF_STALE_HANDLE&&sf_file_pread(&c,(1ull<<32)|alias,out,1,0)==SF_STALE_HANDLE&&sends==before);
 actor=43;saved=ns;CHECK(sf_file_pread(&c,alias,out,1,0)==SF_STALE_HANDLE&&!memcmp(&saved,&ns,sizeof(ns)));actor=42;
 int write=sf_ns_open(&ns,42,"/","/data/item",OPEN_WRITE);CHECK(write>0&&sf_file_pread(&c,write,out,0,0)==SF_DENIED);CHECK(!sf_ns_close(&ns,42,write));
 CHECK(sf_file_pread(&c,root,out,1,0)==SF_INVALID);
 for(unsigned i=0;i<2;i++){
  int special=sf_file_openat(&c,root,i?"console":"pipe",OPEN_READ);CHECK(special>0);
  CHECK(sf_file_pread(&c,special,out,1,0)==SF_NOT_SEEKABLE&&sf_file_pread(&c,special,0,0,0)==SF_NOT_SEEKABLE);CHECK(!sf_file_close(&c,special));
 }
 raw((1ull<<32)|alias,1,0,0,SF_STALE_HANDLE);raw(alias,257,0,0,SF_INVALID);raw(alias,UINT32_MAX,0,0,SF_INVALID);raw(alias,1,1,0,SF_INVALID);
 /* Actual backend sees the full 64-bit offset, even at EOF; no stale cache shortcut. */
 int disk=sf_file_openat(&c,root,"disk",OPEN_READ);CHECK(disk>0&&sf_file_seek(&c,disk,7)==7);saved=ns;
 CHECK(sf_file_pread(&c,disk,out,5,20)==5&&!memcmp(out,expected+20,5)&&backend_calls==1&&backend_offset==20&&!memcmp(&saved,&ns,sizeof(ns)));
 CHECK(sf_file_pread(&c,disk,out,1,UINT64_MAX)==0&&backend_offset==UINT64_MAX&&!memcmp(&saved,&ns,sizeof(ns)));
 backend_mode=4;CHECK(sf_file_pread(&c,disk,out,8,3)==2&&!memcmp(out,expected+3,2)&&!memcmp(&saved,&ns,sizeof(ns)));
 backend_mode=1;memset(out,0xa5,sizeof(out));CHECK(sf_file_pread(&c,disk,out,8,0)==SF_DEVICE_GONE&&!memcmp(&saved,&ns,sizeof(ns)));for(unsigned i=0;i<sizeof(out);i++)CHECK(out[i]==0xa5);
 CHECK(sf_file_pread(&c,disk,out,1,UINT64_MAX)==SF_DEVICE_GONE&&backend_offset==UINT64_MAX);
 backend_mode=2;CHECK(sf_file_pread(&c,disk,out,8,0)==SF_BAD_FORMAT&&!memcmp(&saved,&ns,sizeof(ns)));
 backend_mode=3;CHECK(sf_file_pread(&c,disk,out,1,UINT64_MAX)==SF_BAD_FORMAT&&!memcmp(&saved,&ns,sizeof(ns)));
 backend_mode=0;ns.external_read=0;CHECK(sf_file_pread(&c,disk,out,1,0)==SF_DEVICE_GONE);ns.external_read=disk_read;
 CHECK(!sf_file_stat(&c,disk,&info)&&info.offset==7&&!sf_file_close(&c,disk));
 /* Invalid success metadata never reaches output and stops further sends. */
 for(corrupt=1;corrupt<=6;corrupt++){
  SfFileClient broken=client();memset(out,0xa5,sizeof(out));saved=ns;
  CHECK(sf_file_pread(&broken,alias,out,8,4)==SF_BAD_FORMAT&&broken.error==SF_BAD_FORMAT&&!memcmp(&saved,&ns,sizeof(ns)));
  for(unsigned i=0;i<sizeof(out);i++)CHECK(out[i]==0xa5);
  before=sends;CHECK(sf_file_pread(&broken,alias,out,1,0)==SF_BAD_FORMAT&&sends==before);
 }
 corrupt=0;CHECK(!sf_file_close(&c,alias));saved=ns;CHECK(sf_file_pread(&c,alias,out,1,0)==SF_STALE_HANDLE&&!memcmp(&saved,&ns,sizeof(ns)));
 printf("FILE PREAD PASS: %u checks; 1000 positional byte vectors, shared cursor unchanged, owner/rights/64-bit EOF, backend failures and malformed replies\n",checks);return 0;
}
