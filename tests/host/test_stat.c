#include <stdio.h>
#include "file_rpc.h"
static SfNamespace ns,saved;static unsigned checks,backend_calls;
#define CHECK(x) do{checks++;if(!(x)){printf("STAT FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static long external_read(unsigned index,uint64_t offset,void*out,size_t count){(void)index;(void)offset;(void)out;(void)count;backend_calls++;return SF_DEVICE_GONE;}
static int node(const char*p){for(unsigned i=0;i<32;i++)if(ns.nodes[i].type&&!strcmp(ns.nodes[i].path,p))return i;return -1;}
static struct{uint8_t left[32];SfMessage m;uint8_t right[32];}guard;
static void request(unsigned op,uint64_t handle,unsigned length){memset(&guard,0xa5,sizeof(guard));memset(&guard.m,0,sizeof(guard.m));guard.m.h.opcode=op;guard.m.handle=handle;guard.m.length=length;}
static int guards(void){for(unsigned i=0;i<32;i++)if(guard.left[i]!=0xa5||guard.right[i]!=0xa5)return 0;return 1;}
int main(void){
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/cap",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/cap/item",NODE_FILE,0));CHECK(!sf_ns_create(&ns,"/cap/pipe",NODE_PIPE,0));CHECK(!sf_ns_create(&ns,"/cap/console",NODE_CONSOLE,0));CHECK(!sf_ns_create(&ns,"/cap/disk",NODE_DISK,0));
 int root=sf_ns_open(&ns,42,"/","/cap",OPEN_READ|OPEN_WRITE|OPEN_DIRECTORY),write=sf_ns_openat(&ns,42,root,"item",OPEN_WRITE),read=sf_ns_openat(&ns,42,root,"item",OPEN_READ);CHECK(root>0&&write>0&&read>0);
 SfFileStat info,previous;memset(&info,0xa5,sizeof(info));previous=info;saved=ns;
 CHECK(sf_ns_stat(&ns,43,read,&info)==SF_STALE_HANDLE&&!memcmp(&info,&previous,sizeof(info)));CHECK(!memcmp(&ns,&saved,sizeof(ns)));CHECK(sf_ns_stat(&ns,42,read^256,&info)==SF_STALE_HANDLE&&!memcmp(&info,&previous,sizeof(info)));CHECK(sf_ns_stat(&ns,42,read,0)==SF_INVALID);
 CHECK(!sf_ns_stat(&ns,42,write,&info));CHECK(info.bytes==32&&info.type==NODE_FILE&&info.mode==OPEN_WRITE&&info.flags==SF_FILE_SEEKABLE&&info.size==0&&info.offset==0);
 CHECK(sf_ns_write(&ns,42,write,"abcdef",6)==6);CHECK(!sf_ns_stat(&ns,42,write,&info)&&info.size==6&&info.offset==6);CHECK(!sf_ns_stat(&ns,42,read,&info)&&info.size==6&&info.offset==0);
 int duplicate=sf_ns_dup(&ns,42,read);CHECK(duplicate>0);char out[8];CHECK(sf_ns_read(&ns,42,read,out,2)==2);CHECK(!sf_ns_stat(&ns,42,duplicate,&info)&&info.offset==2&&info.size==6);CHECK(!sf_ns_truncate(&ns,42,write,1));CHECK(!sf_ns_stat(&ns,42,read,&info)&&info.offset==2&&info.size==1);
 unsigned item=node("/cap/item");ns.nodes[item].readonly=1;CHECK(!sf_ns_stat(&ns,42,read,&info)&&info.flags==(SF_FILE_READ_ONLY|SF_FILE_SEEKABLE)&&info.mode==OPEN_READ);ns.nodes[item].readonly=0;
 CHECK(!sf_ns_stat(&ns,42,root,&info)&&info.type==NODE_DIR&&info.mode==(OPEN_READ|OPEN_WRITE|OPEN_DIRECTORY)&&!info.size&&!info.offset&&!info.flags);
 int pipe=sf_ns_openat(&ns,42,root,"pipe",OPEN_READ|OPEN_WRITE),console=sf_ns_openat(&ns,42,root,"console",OPEN_WRITE),disk=sf_ns_openat(&ns,42,root,"disk",OPEN_READ);CHECK(pipe>0&&console>0&&disk>0);
 CHECK(sf_ns_write(&ns,42,pipe,"queue",5)==5);CHECK(!sf_ns_stat(&ns,42,pipe,&info)&&info.type==NODE_PIPE&&info.size==5&&!info.flags&&!info.offset);CHECK(sf_ns_read(&ns,42,pipe,out,2)==2);CHECK(!sf_ns_stat(&ns,42,pipe,&info)&&info.size==3);
 CHECK(!sf_ns_stat(&ns,42,console,&info)&&info.type==NODE_CONSOLE&&!info.size&&!info.offset&&!info.flags);
 unsigned index=node("/cap/disk");ns.nodes[index].size=16384;ns.nodes[index].readonly=1;ns.nodes[index].mount_tag=7;ns.external_read=external_read;saved=ns;
 CHECK(!sf_ns_stat(&ns,42,disk,&info));CHECK(info.type==NODE_DISK&&info.size==16384&&info.flags==7&&info.mode==OPEN_READ);CHECK(!backend_calls&&!memcmp(&ns,&saved,sizeof(ns)));CHECK(sf_ns_read(&ns,42,disk,out,1)==SF_DEVICE_GONE&&backend_calls==1);CHECK(!sf_ns_stat(&ns,42,disk,&info)&&info.size==16384&&backend_calls==1);
 /* Metadata never alters offsets, and duplicate descriptions observe each
    other's movement while independent opens keep their own offsets. */
 for(unsigned i=0;i<1000;i++){
  unsigned size=i%1025,offset=(i*7)%1025;CHECK(!sf_ns_truncate(&ns,42,write,size));CHECK(sf_ns_seek(&ns,42,read,offset)==(long)offset);saved=ns;CHECK(!sf_ns_stat(&ns,42,duplicate,&info));CHECK(info.bytes==32&&info.size==size&&info.offset==offset&&info.mode==OPEN_READ&&info.flags==SF_FILE_SEEKABLE);CHECK(!memcmp(&ns,&saved,sizeof(ns)));CHECK(!sf_ns_stat(&ns,42,write,&info)&&info.size==size&&info.offset==6);
 }
 uint32_t owned=root;unsigned operations[]={OP_READ,OP_WRITE,OP_CLOSE,OP_SEEK,OP_POLL,OP_ENUMERATE,OP_OPENAT,OP_TRUNCATE,OP_UNLINK,OP_STAT};uint64_t high[]={1ull<<32,1ull<<63,0xffffffff00000000ull};
 for(unsigned i=0;i<sizeof(operations)/sizeof(*operations);i++)for(unsigned j=0;j<sizeof(high)/sizeof(*high);j++){
  request(operations[i],high[j]|(unsigned)root,5);guard.m.mode=OPEN_READ;memcpy(guard.m.data,"item",5);saved=ns;uint64_t original=guard.m.handle;
  CHECK(sf_file_dispatch(&ns,42,&owned,&guard.m)==SF_STALE_HANDLE);CHECK(guard.m.status==SF_STALE_HANDLE&&!guard.m.length&&guard.m.handle==original);CHECK(owned==(unsigned)root&&!memcmp(&ns,&saved,sizeof(ns))&&guards());
 }
 request(OP_ROOT,UINT64_MAX,0);CHECK(!sf_file_dispatch(&ns,42,&owned,&guard.m)&&guard.m.handle==owned);
 unsigned bad_lengths[]={1,32,256,257,UINT32_MAX};for(unsigned i=0;i<sizeof(bad_lengths)/sizeof(*bad_lengths);i++){
  request(OP_STAT,read,bad_lengths[i]);saved=ns;CHECK(sf_file_dispatch(&ns,42,&owned,&guard.m)==SF_INVALID);CHECK(!guard.m.length&&guard.m.handle==(unsigned)read&&guards());CHECK(!memcmp(&ns,&saved,sizeof(ns))&&owned==(unsigned)root);
 }
 request(OP_STAT,read,0);saved=ns;CHECK(!sf_file_dispatch(&ns,42,&owned,&guard.m));CHECK(guard.m.length==sizeof(info)&&guard.m.handle==(unsigned)read&&guards());memcpy(&info,guard.m.data,sizeof(info));CHECK(info.bytes==32&&info.mode==OPEN_READ&&info.type==NODE_FILE);CHECK(!memcmp(&ns,&saved,sizeof(ns)));
 request(OP_STAT,read,0);saved=ns;CHECK(sf_file_dispatch(&ns,43,&owned,&guard.m)==SF_STALE_HANDLE&&!guard.m.length);CHECK(!memcmp(&ns,&saved,sizeof(ns))&&guards());
 CHECK(!sf_ns_close(&ns,42,root));memset(&info,0xa5,sizeof(info));previous=info;CHECK(sf_ns_stat(&ns,42,root,&info)==SF_STALE_HANDLE&&!memcmp(&info,&previous,sizeof(info)));sf_ns_reap(&ns,42);CHECK(sf_ns_stat(&ns,42,read,&info)==SF_STALE_HANDLE);for(unsigned i=0;i<32;i++)CHECK(!ns.descriptions[i].refs);
 printf("STAT PASS: %u assertions\n",checks);return 0;
}
