#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SF_FILE_SYSCALL dup_syscall
#include "../../services/file_client.c"
#include "file_rpc.h"
static SfNamespace ns,snapshot;
static uint32_t roots[2],actor=42;
static SfMessage reply;
static unsigned checks,sends,calls,corrupt,opens,closes,console_bytes;
static uint64_t original_handle;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FILE DUP FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static long console(const void*p,size_t n){CHECK(p&&n<=256);console_bytes+=n;return(long)n;}
static int mount_open(unsigned tag){CHECK(tag==1);opens++;return 0;}
static void mount_close(unsigned tag){CHECK(tag==1);closes++;}
static void setup(void){
 sf_ns_init(&ns,console);CHECK(!sf_ns_create(&ns,"/data",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/data/item",NODE_FILE,0));
 CHECK(!sf_ns_create(&ns,"/data/pipe",NODE_PIPE,0));CHECK(!sf_ns_create(&ns,"/mnt",NODE_DIR,0));
 CHECK(!sf_ns_create(&ns,"/mnt/item",NODE_DISK,0));CHECK(!sf_ns_create(&ns,"/dev",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/dev/console",NODE_CONSOLE,0));
 ns.nodes[5].mount_tag=1;ns.nodes[5].size=6;ns.mount_open=mount_open;ns.mount_close=mount_close;
 roots[0]=sf_ns_open(&ns,42,"/","/data",OPEN_READ|OPEN_WRITE|OPEN_DIRECTORY);
 roots[1]=sf_ns_open(&ns,43,"/","/data",OPEN_READ|OPEN_DIRECTORY);
 CHECK((int)roots[0]>0&&(int)roots[1]>0);actor=42;corrupt=0;opens=closes=console_bytes=0;
}
static SfFileClient client(void){SfFileClient c;CHECK(!sf_file_client_init(&c,0x101,0x102,3000));return c;}
int64_t dup_syscall(uint64_t op,uint64_t a,uint64_t b,uint64_t deadline){
 calls++;if(op==SYS_TICKS)return 10;
 SfMessage*m=(void*)(uintptr_t)b;
 if(op==SYS_SEND){
  CHECK(a==0x101&&!deadline);CHECK(m->h.protocol_major==1&&!m->h.protocol_minor&&m->h.payload_bytes==sizeof(*m)-40);
  sends++;original_handle=m->handle;reply=*m;sf_file_dispatch(&ns,actor,&roots[actor==42?0:1],&reply);reply.h.flags=1;return 0;
 }
 CHECK(op==SYS_RECV&&a==0x102&&deadline==reply.h.deadline_ns);*m=reply;
 if(corrupt&&m->h.opcode==OP_DUP)switch(corrupt){
  case 1:m->handle=0;break;case 2:m->handle|=1ull<<32;break;case 3:m->handle=original_handle;break;
  case 4:m->length=1;break;case 5:m->mode=1;break;case 6:m->offset=1;break;
  case 7:m->status=1;break;case 8:return 2;case 9:m->h.flags=2;break;case 10:m->h.opcode=OP_READ;break;
 }
 return 1;
}
static uint32_t raw_dup(uint64_t h,uint32_t length,uint32_t mode,uint64_t offset,long expected){
 SfMessage m={0};m.h.opcode=OP_DUP;m.handle=h;m.length=length;m.mode=mode;m.offset=offset;
 long r=sf_file_dispatch(&ns,actor,&roots[actor==42?0:1],&m);CHECK(r==expected&&m.status==expected);return(uint32_t)m.handle;
}
int main(void){
 setup();SfFileClient c=client();uint32_t root=roots[0];
 int file=sf_ns_openat(&ns,42,root,"item",OPEN_READ|OPEN_WRITE);CHECK(file>0);
 CHECK(sf_ns_write(&ns,42,file,"abcdef",6)==6&&sf_ns_seek(&ns,42,file,0)==0);
 int64_t copy=sf_file_dup(&c,file);CHECK(copy>0&&copy!=file&&!c.error);
 SfFileStat info;CHECK(!sf_file_stat(&c,copy,&info)&&info.mode==(OPEN_READ|OPEN_WRITE));
 char data[8];CHECK(sf_file_read(&c,file,data,2)==2&&!memcmp(data,"ab",2));
 CHECK(!sf_file_stat(&c,copy,&info)&&info.offset==2);
 CHECK(sf_file_seek(&c,copy,4)==4&&sf_file_read(&c,file,data,2)==2&&!memcmp(data,"ef",2));
 CHECK(!sf_file_close(&c,file)&&sf_file_stat(&c,file,&info)==SF_STALE_HANDLE);
 CHECK(sf_file_unlink(&c,root,"item")==SF_BUSY);
 CHECK(sf_file_seek(&c,copy,1)==1&&sf_file_read(&c,copy,data,2)==2&&!memcmp(data,"bc",2));
 CHECK(!sf_file_close(&c,copy));
 /* Default-root lookup stays closed; the explicitly duplicated directory lives. */
 int64_t directory=sf_file_dup(&c,root);CHECK(directory>0&&directory!=root);
 CHECK(!sf_file_close(&c,root)&&!roots[0]&&sf_file_root(&c)==SF_DENIED);
 file=sf_file_openat(&c,directory,"item",OPEN_READ);CHECK(file>0&&sf_file_read(&c,file,data,1)==1&&data[0]=='a');
 CHECK(!sf_file_close(&c,file)&&!sf_file_close(&c,directory)&&sf_file_root(&c)==SF_DENIED);
 /* Owner and rights attenuation remain those of the original description. */
 actor=43;c=client();file=sf_file_openat(&c,roots[1],"item",OPEN_READ);CHECK(file>0);
 copy=sf_file_dup(&c,file);CHECK(copy>0&&sf_file_write(&c,copy,"x",1)==SF_DENIED&&sf_file_truncate(&c,copy,0)==SF_DENIED);
 CHECK(!sf_file_close(&c,file)&&sf_file_read(&c,copy,data,1)==1&&data[0]=='a');
 CHECK(!sf_file_close(&c,copy));actor=42;
 file=sf_ns_open(&ns,42,"/","/data/item",OPEN_READ);CHECK(file>0);actor=43;
 snapshot=ns;CHECK(sf_file_dup(&c,file)==SF_STALE_HANDLE&&!memcmp(&snapshot,&ns,sizeof(ns))&&!c.error);actor=42;
 /* Malformed operands are rejected before allocating any reference. */
 const uint64_t invalid[]={0,(1ull<<32)|(uint32_t)file,UINT64_MAX};
 for(unsigned i=0;i<3;i++){snapshot=ns;raw_dup(invalid[i],0,0,0,SF_STALE_HANDLE);CHECK(!memcmp(&snapshot,&ns,sizeof(ns)));}
 for(unsigned field=0;field<3;field++){
  snapshot=ns;raw_dup(file,field==0?1:0,field==1?OPEN_WRITE:0,field==2?1:0,SF_INVALID);CHECK(!memcmp(&snapshot,&ns,sizeof(ns)));
 }
 c=client();unsigned prior=sends;CHECK(sf_file_dup(&c,0)==SF_STALE_HANDLE&&sf_file_dup(&c,(1ull<<32)|file)==SF_STALE_HANDLE&&sends==prior&&!c.error);
 /* One mounted description invokes its close callback only at final release. */
 setup();c=client();file=sf_ns_open(&ns,42,"/","/mnt/item",OPEN_READ);CHECK(file>0&&opens==1&&!closes);
 copy=sf_file_dup(&c,file);CHECK(copy>0&&opens==1&&!closes);
 CHECK(!sf_file_close(&c,file)&&!closes&&sf_ns_unmount(&ns,1)==SF_BUSY);
 CHECK(!sf_file_close(&c,copy)&&closes==1);
 file=sf_ns_open(&ns,42,"/","/mnt/item",OPEN_READ);CHECK(file>0);CHECK(sf_file_dup(&c,file)>0);
 sf_ns_reap(&ns,42);CHECK(opens==2&&closes==2&&!sf_ns_unmount(&ns,1));
 /* A duplicate writer is one description: EOF arrives after its last handle. */
 setup();c=client();int reader=sf_ns_openat(&ns,42,roots[0],"pipe",OPEN_READ);
 int writer=sf_ns_openat(&ns,42,roots[0],"pipe",OPEN_WRITE);CHECK(reader>0&&writer>0);
 copy=sf_file_dup(&c,writer);CHECK(copy>0&&!sf_file_close(&c,writer));
 CHECK(sf_file_read(&c,reader,data,1)==SF_WOULD_BLOCK);
 CHECK(sf_file_write(&c,copy,"q",1)==1&&sf_file_read(&c,reader,data,1)==1&&data[0]=='q');
 CHECK(!sf_file_close(&c,copy)&&sf_file_read(&c,reader,data,1)==0&&!sf_file_close(&c,reader));
 int64_t output=sf_file_console(&c);CHECK(output>0);copy=sf_file_dup(&c,output);CHECK(copy>0&&!sf_file_close(&c,output));
 CHECK(sf_file_write(&c,copy,"ok",2)==2&&console_bytes==2&&!sf_file_close(&c,copy));
 /* Retired/free slots cannot be reused to manufacture a stale reference. */
 setup();c=client();file=sf_ns_openat(&ns,42,roots[0],"item",OPEN_READ);CHECK(file>0);
 for(unsigned i=0;i<64;i++)if(!ns.handles[i].description)ns.handles[i].generation=0x7fffff;
 snapshot=ns;CHECK(sf_file_dup(&c,file)==SF_NO_SPACE&&!memcmp(&snapshot,&ns,sizeof(ns))&&!c.error);
 /* Repeated duplication never grows the description or mount reference count. */
 setup();c=client();file=sf_ns_openat(&ns,42,roots[0],"item",OPEN_READ|OPEN_WRITE);CHECK(file>0);
 for(unsigned i=0;i<1000;i++){
  copy=sf_file_dup(&c,file);CHECK(copy>0&&copy!=file);CHECK(sf_file_seek(&c,copy,0)==0);
  CHECK(sf_file_write(&c,file,"z",1)==1&&!sf_file_stat(&c,copy,&info)&&info.offset==1);
  CHECK(!sf_file_close(&c,copy)&&sf_file_stat(&c,copy,&info)==SF_STALE_HANDLE);
 }
 CHECK(!sf_file_close(&c,file));
 for(unsigned i=0;i<32;i++)CHECK(!ns.descriptions[i].refs||ns.descriptions[i].node==1);
 /* Authenticated malformed success replies poison only the client; reaping
  * still releases references created before the reply was lost/corrupted. */
 for(unsigned bad=1;bad<=10;bad++){
  setup();c=client();file=sf_ns_openat(&ns,42,roots[0],"item",OPEN_READ);CHECK(file>0);
  corrupt=bad;
  CHECK(sf_file_dup(&c,file)==SF_BAD_FORMAT&&c.error==SF_BAD_FORMAT);
  prior=calls;CHECK(sf_file_dup(&c,file)==SF_BAD_FORMAT&&calls==prior);
  sf_ns_reap(&ns,42);for(unsigned i=0;i<64;i++)CHECK(!ns.handles[i].description||ns.handles[i].owner!=42);
 }
 printf("FILE DUP PASS: %u checks; same-owner shared cursor/rights, root aliases, mount/pipe lifetime, exhaustion, 1000 reuses and malformed replies\n",checks);
 return 0;
}
