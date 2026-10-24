#include <stdio.h>
#include "file_rpc.h"
static SfNamespace ns,snapshot;static unsigned checks,console_calls;static size_t console_bytes;
static long console(const void*p,size_t n){(void)p;console_calls++;console_bytes=n;return n;}
static struct {uint8_t before[64];SfMessage m;uint8_t after[2048];} guarded;
static void request(unsigned op,uint32_t handle,uint32_t length){memset(&guarded,0xa5,sizeof(guarded));memset(&guarded.m,0,sizeof(guarded.m));guarded.m.h.opcode=op;guarded.m.handle=handle;guarded.m.length=length;}
static int guards(void){for(unsigned i=0;i<sizeof(guarded.before);i++)if(guarded.before[i]!=0xa5)return 0;for(unsigned i=0;i<sizeof(guarded.after);i++)if(guarded.after[i]!=0xa5)return 0;return 1;}
#define CHECK(x) do{checks++;if(!(x)){printf("FILE RPC FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 sf_ns_init(&ns,console);CHECK(!sf_ns_create(&ns,"/data",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/data/item",NODE_FILE,0));CHECK(!sf_ns_create(&ns,"/dev",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/dev/console",NODE_CONSOLE,0));
 uint32_t root=sf_ns_open(&ns,42,"/","/data",OPEN_READ|OPEN_DIRECTORY);CHECK((int)root>0);int file=sf_ns_open(&ns,42,"/","/data/item",OPEN_READ|OPEN_WRITE),output=sf_ns_open(&ns,42,"/","/dev/console",OPEN_WRITE);CHECK(file>0&&output>0);
 uint8_t content[1024];for(unsigned i=0;i<sizeof(content);i++)content[i]='A'+i/256;CHECK(sf_ns_write(&ns,42,file,content,sizeof(content))==1024);CHECK(!sf_ns_seek(&ns,42,file,0));
 /* Enumeration is intentionally first: the old product dispatcher fails this
  * assertion without exercising an unsafe overflowing read in the host.
  * The host bypasses SYS_SEND: the existing kernel already refuses these
  * sizes, so baseline failure shows missing local defense, not reachability. */
 unsigned ops[]={OP_ENUMERATE,OP_READ,OP_WRITE};uint32_t lengths[]={257,512,0x80000000u,UINT32_MAX};
 for(unsigned i=0;i<3;i++)for(unsigned j=0;j<4;j++){
  request(ops[i],i==0?root:i==1?(uint32_t)file:(uint32_t)output,lengths[j]);snapshot=ns;uint32_t saved_root=root;
  CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==SF_INVALID);CHECK(guarded.m.status==SF_INVALID&&guarded.m.length==0);CHECK(!memcmp(&ns,&snapshot,sizeof(ns))&&root==saved_root);CHECK(!console_calls&&guards());
 }
 request(OP_READ,file,256);CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==256);CHECK(guarded.m.length==256&&guards());for(unsigned i=0;i<256;i++)CHECK(guarded.m.data[i]=='A');
 request(OP_READ,file,257);CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==SF_INVALID);
 request(OP_READ,file,1);CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==1&&guarded.m.data[0]=='B'&&guards());
 request(OP_READ,file,0);CHECK(!sf_file_dispatch(&ns,42,&root,&guarded.m));CHECK(sf_ns_seek(&ns,42,file,0)==0);
 request(OP_WRITE,file,256);memset(guarded.m.data,'W',256);CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==256&&guards());
 request(OP_WRITE,file,257);snapshot=ns;CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==SF_INVALID);CHECK(!memcmp(&ns,&snapshot,sizeof(ns))&&guards());
 request(OP_WRITE,output,256);CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==256);CHECK(console_calls==1&&console_bytes==256&&guards());
 request(OP_WRITE,output,0);CHECK(!sf_file_dispatch(&ns,42,&root,&guarded.m)&&console_calls==1);
 request(OP_ENUMERATE,root,256);CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==5);CHECK(guarded.m.length==5&&!memcmp(guarded.m.data,"item\n",5)&&guards());
 for(unsigned i=0;i<8;i++){char path[80];snprintf(path,sizeof(path),"/data/long-entry-%02u-abcdefghijklmnopqrstuvwx",i);CHECK(!sf_ns_create(&ns,path,NODE_FILE,0));}
 request(OP_ENUMERATE,root,256);long listed=sf_file_dispatch(&ns,42,&root,&guarded.m);CHECK(listed>200&&listed<=256);CHECK(guarded.m.length==(unsigned)listed&&guards());
 request(OP_ROOT,0,0);CHECK(!sf_file_dispatch(&ns,42,&root,&guarded.m)&&guarded.m.handle==root);
 request(OP_READ,file,1);CHECK(sf_file_dispatch(&ns,99,&root,&guarded.m)==SF_STALE_HANDLE&&guards());
 request(OP_OPENAT,root,5);memcpy(guarded.m.data,"item",5);guarded.m.mode=OPEN_READ;CHECK(!sf_file_dispatch(&ns,42,&root,&guarded.m));int child=guarded.m.handle;CHECK(child>0&&guards());
 request(OP_TRUNCATE,child,0);guarded.m.offset=0;CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==SF_DENIED);
 const uint32_t bad_paths[]={0,257,UINT32_MAX};for(unsigned i=0;i<3;i++){request(OP_OPENAT,root,bad_paths[i]);snapshot=ns;CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==SF_INVALID);CHECK(!memcmp(&ns,&snapshot,sizeof(ns))&&guards());}
 request(OP_OPENAT,root,11);memcpy(guarded.m.data,"item\0extra",11);CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==SF_INVALID&&guards());
 request(OP_OPEN,0,11);memcpy(guarded.m.data,"/data/item",11);guarded.m.mode=OPEN_READ;CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==SF_DENIED);
 request(OP_CLOSE,root,0);CHECK(!sf_file_dispatch(&ns,42,&root,&guarded.m)&&!root);
 request(OP_READ,child,1);CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==1&&guarded.m.data[0]=='W');
 request(OP_ROOT,0,0);CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==SF_DENIED);
 request(0xffffffffu,0,0);CHECK(sf_file_dispatch(&ns,42,&root,&guarded.m)==SF_INVALID&&guards());
 sf_ns_reap(&ns,42);printf("FILE RPC PASS: %u checks; bounded copied payload, canaries, unchanged offsets/state, rights and delegated-root lifecycle\n",checks);return 0;
}
