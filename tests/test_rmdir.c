#include <stdio.h>
#include "file_rpc.h"
static SfNamespace ns,saved;static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("RMDIR FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define REJECT(expr,error) do{saved=ns;CHECK((expr)==(error));CHECK(!memcmp(&ns,&saved,sizeof(ns)));}while(0)
static int find(const char*p){for(unsigned i=0;i<32;i++)if(ns.nodes[i].type&&!strcmp(ns.nodes[i].path,p))return i;return -1;}
int main(void){
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/cap",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/cap/sub",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/cap/sub/item",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/cap/sub/item2",NODE_DIR,0));
 int root=sf_ns_open(&ns,42,"/","/cap",OPEN_READ|OPEN_WRITE|OPEN_DIRECTORY),read=sf_ns_open(&ns,43,"/","/cap",OPEN_READ|OPEN_DIRECTORY),global=sf_ns_open(&ns,1,"/","/",OPEN_WRITE|OPEN_DIRECTORY);CHECK(root>0&&read>0&&global>0);
 REJECT(sf_ns_rmdir(&ns,43,root,"sub/item"),SF_STALE_HANDLE);REJECT(sf_ns_rmdir(&ns,42,root^256,"sub/item"),SF_STALE_HANDLE);REJECT(sf_ns_rmdir(&ns,42,0,"sub/item"),SF_STALE_HANDLE);REJECT(sf_ns_rmdir(&ns,43,read,"sub/item"),SF_DENIED);
 int held=sf_ns_openat(&ns,42,root,"sub/item",OPEN_READ|OPEN_DIRECTORY);CHECK(held>0);int duplicate=sf_ns_dup(&ns,42,held),other=sf_ns_open(&ns,9,"/","/cap/sub/item",OPEN_READ|OPEN_DIRECTORY);CHECK(duplicate>0&&other>0);
 REJECT(sf_ns_rmdir(&ns,42,root,"sub/item"),SF_BUSY);CHECK(!sf_ns_close(&ns,42,held));REJECT(sf_ns_rmdir(&ns,42,root,"sub/item"),SF_BUSY);CHECK(!sf_ns_close(&ns,42,duplicate));REJECT(sf_ns_rmdir(&ns,42,root,"sub/item"),SF_BUSY);sf_ns_reap(&ns,9);
 CHECK(!sf_ns_rmdir(&ns,42,root,"sub/item"));CHECK(find("/cap/sub/item")<0&&find("/cap/sub/item2")>=0);REJECT(sf_ns_rmdir(&ns,42,root,"sub/item"),SF_NOT_FOUND);char out[64];CHECK(sf_ns_enumerate(&ns,42,held,out,sizeof(out))==SF_STALE_HANDLE);
 CHECK(!sf_ns_create(&ns,"/cap/sub/item",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/cap/sub/item/file",NODE_FILE,0));REJECT(sf_ns_rmdir(&ns,42,root,"sub/item"),SF_BUSY);CHECK(!sf_ns_unlink(&ns,42,root,"sub/item/file"));CHECK(!sf_ns_create(&ns,"/cap/sub/item/deep",NODE_DIR,0));REJECT(sf_ns_rmdir(&ns,42,root,"sub/item"),SF_BUSY);CHECK(!sf_ns_create(&ns,"/cap/sub/item/deep/file",NODE_FILE,0));REJECT(sf_ns_rmdir(&ns,42,root,"sub/item"),SF_BUSY);
 CHECK(!sf_ns_unlink(&ns,42,root,"sub/item/deep/file"));CHECK(!sf_ns_rmdir(&ns,42,root,"sub/item/deep"));
 const char*bad[]={"","/cap/sub/item",".","..","../cap","sub/../item","sub//item","sub/","sub/./item","\xc0\xaf","sub/\x01","\xe7"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)REJECT(sf_ns_rmdir(&ns,42,root,bad[i]),SF_INVALID);
 char too_long[97];memset(too_long,'x',96);too_long[96]=0;REJECT(sf_ns_rmdir(&ns,42,root,too_long),SF_INVALID);REJECT(sf_ns_rmdir(&ns,42,root,"missing/item"),SF_NOT_FOUND);
 int base=find("/cap"),parent=find("/cap/sub"),target=find("/cap/sub/item");ns.nodes[base].readonly=1;REJECT(sf_ns_rmdir(&ns,42,root,"sub/item"),SF_READ_ONLY);ns.nodes[base].readonly=0;ns.nodes[parent].readonly=1;REJECT(sf_ns_rmdir(&ns,1,global,"cap/sub/item"),SF_READ_ONLY);ns.nodes[parent].readonly=0;
 ns.nodes[parent].mount_tag=7;REJECT(sf_ns_rmdir(&ns,42,root,"sub/item"),SF_READ_ONLY);ns.nodes[parent].mount_tag=0;ns.nodes[target].readonly=1;REJECT(sf_ns_rmdir(&ns,42,root,"sub/item"),SF_READ_ONLY);ns.nodes[target].readonly=0;ns.nodes[target].mount_tag=7;REJECT(sf_ns_rmdir(&ns,42,root,"sub/item"),SF_READ_ONLY);ns.nodes[target].mount_tag=0;ns.nodes[base].mount_tag=7;REJECT(sf_ns_rmdir(&ns,42,root,"sub/item"),SF_READ_ONLY);ns.nodes[base].mount_tag=0;CHECK(!sf_ns_rmdir(&ns,42,root,"sub/item"));
 for(unsigned type=NODE_FILE;type<=NODE_DISK;type++){CHECK(!sf_ns_create(&ns,"/cap/special",type,0));REJECT(sf_ns_rmdir(&ns,42,root,"special"),SF_INVALID);memset(&ns.nodes[find("/cap/special")],0,sizeof(SfNode));}
 CHECK(!sf_ns_create(&ns,"/cap/spaced dir",NODE_DIR,0));CHECK(!sf_ns_rmdir(&ns,42,root,"spaced dir"));CHECK(!sf_ns_create(&ns,"/cap/\xe7\x8b\x90",NODE_DIR,0));CHECK(!sf_ns_rmdir(&ns,42,root,"\xe7\x8b\x90"));
 char max_path[96]="/cap/";memset(max_path+5,'a',90);max_path[95]=0;CHECK(!sf_ns_create(&ns,max_path,NODE_DIR,0));CHECK(!sf_ns_rmdir(&ns,42,root,max_path+5));
 for(unsigned i=0;i<1000;i++){
  CHECK(!sf_ns_create(&ns,"/cap/recycled",NODE_DIR,0));int h=sf_ns_openat(&ns,42,root,"recycled",OPEN_READ|OPEN_DIRECTORY);CHECK(h>0&&h!=held);REJECT(sf_ns_rmdir(&ns,42,root,"recycled"),SF_BUSY);
  CHECK(!sf_ns_close(&ns,42,h));CHECK(!sf_ns_create(&ns,"/cap/recycled/child",NODE_FILE,0));REJECT(sf_ns_rmdir(&ns,42,root,"recycled"),SF_BUSY);CHECK(!sf_ns_unlink(&ns,42,root,"recycled/child"));CHECK(!sf_ns_rmdir(&ns,42,root,"recycled"));CHECK(sf_ns_stat(&ns,42,h,(SfFileStat*)out)==SF_STALE_HANDLE);
 }
 CHECK(!sf_ns_create(&ns,"/cap/empty",NODE_DIR,0));struct{uint8_t left[32];SfMessage m;uint8_t right[32];}g;uint32_t owned=root;
 unsigned lengths[]={0,257,0x80000000u,UINT32_MAX,5,13};
 for(unsigned i=0;i<sizeof(lengths)/sizeof(*lengths);i++){
  memset(&g,0xa5,sizeof(g));memset(&g.m,0,sizeof(g.m));g.m.h.opcode=OP_RMDIR;g.m.handle=root;g.m.length=lengths[i];memcpy(g.m.data,"empty\0extra",12);saved=ns;
  CHECK(sf_file_dispatch(&ns,42,&owned,&g.m)==SF_INVALID);CHECK(g.m.status==SF_INVALID&&g.m.handle==(unsigned)root&&owned==(unsigned)root);CHECK(!memcmp(&ns,&saved,sizeof(ns)));for(unsigned j=0;j<32;j++)CHECK(g.left[j]==0xa5&&g.right[j]==0xa5);
 }
 uint64_t masks[]={1ull<<32,1ull<<63,0xffffffff00000000ull};
 for(unsigned i=0;i<3;i++){memset(&g.m,0,sizeof(g.m));g.m.h.opcode=OP_RMDIR;g.m.handle=masks[i]|(uint32_t)root;g.m.length=6;memcpy(g.m.data,"empty",6);REJECT(sf_file_dispatch(&ns,42,&owned,&g.m),SF_STALE_HANDLE);CHECK(!g.m.length&&owned==(unsigned)root);}
 g.m.handle=read;g.m.length=6;REJECT(sf_file_dispatch(&ns,43,&owned,&g.m),SF_DENIED);g.m.handle=root;CHECK(!sf_file_dispatch(&ns,42,&owned,&g.m));CHECK(g.m.handle==(unsigned)root&&owned==(unsigned)root&&find("/cap/empty")<0);
 CHECK(!sf_ns_close(&ns,42,root));REJECT(sf_ns_rmdir(&ns,42,root,"sub"),SF_STALE_HANDLE);sf_ns_reap(&ns,1);sf_ns_reap(&ns,43);for(unsigned i=0;i<32;i++)CHECK(!ns.descriptions[i].refs);
 printf("RMDIR PASS: %u assertions\n",checks);return 0;
}
