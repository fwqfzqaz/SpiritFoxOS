#include <stdio.h>
#include "file_rpc.h"
static SfNamespace ns,saved;static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("UNLINK FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define REJECT(expr,error) do{saved=ns;CHECK((expr)==(error));CHECK(!memcmp(&ns,&saved,sizeof(ns)));}while(0)
static int find(const char*p){for(unsigned i=0;i<32;i++)if(ns.nodes[i].type&&!strcmp(ns.nodes[i].path,p))return i;return -1;}
int main(void){
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/cap",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/cap/sub",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/cap/sub/nested",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/cap/sub/nested/item",NODE_FILE,0));CHECK(!sf_ns_create(&ns,"/cap/item",NODE_FILE,0));
 int root=sf_ns_open(&ns,42,"/","/cap",OPEN_READ|OPEN_WRITE|OPEN_DIRECTORY),read=sf_ns_open(&ns,43,"/","/cap",OPEN_READ|OPEN_DIRECTORY),global=sf_ns_open(&ns,1,"/","/",OPEN_WRITE|OPEN_DIRECTORY);CHECK(root>0&&read>0&&global>0);
 int file=sf_ns_openat(&ns,42,root,"item",OPEN_WRITE|OPEN_READ);CHECK(file>0);CHECK(sf_ns_write(&ns,42,file,"private",7)==7);
 REJECT(sf_ns_unlink(&ns,43,root,"item"),SF_STALE_HANDLE);REJECT(sf_ns_unlink(&ns,42,root^256,"item"),SF_STALE_HANDLE);REJECT(sf_ns_unlink(&ns,42,0,"item"),SF_STALE_HANDLE);REJECT(sf_ns_unlink(&ns,42,file,"item"),SF_INVALID);REJECT(sf_ns_unlink(&ns,43,read,"item"),SF_DENIED);
 REJECT(sf_ns_unlink(&ns,42,root,"item"),SF_BUSY);int duplicate=sf_ns_dup(&ns,42,file),other=sf_ns_open(&ns,9,"/","/cap/item",OPEN_READ);CHECK(duplicate>0&&other>0);CHECK(!sf_ns_close(&ns,42,file));REJECT(sf_ns_unlink(&ns,42,root,"item"),SF_BUSY);CHECK(!sf_ns_close(&ns,42,duplicate));REJECT(sf_ns_unlink(&ns,42,root,"item"),SF_BUSY);CHECK(!sf_ns_close(&ns,9,other));
 CHECK(!sf_ns_unlink(&ns,42,root,"item"));CHECK(find("/cap/item")<0);REJECT(sf_ns_unlink(&ns,42,root,"item"),SF_NOT_FOUND);char bytes[8];CHECK(sf_ns_read(&ns,42,file,bytes,1)==SF_STALE_HANDLE);
 const char*bad[]={"","/cap/item",".","..","../item","sub/../item","sub//item","sub/","sub/./item","\xc0\xaf","sub/\x01","\xe7"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)REJECT(sf_ns_unlink(&ns,42,root,bad[i]),SF_INVALID);
 char long_name[97];memset(long_name,'x',96);long_name[96]=0;REJECT(sf_ns_unlink(&ns,42,root,long_name),SF_INVALID);
 REJECT(sf_ns_unlink(&ns,42,root,"missing/item"),SF_NOT_FOUND);REJECT(sf_ns_unlink(&ns,42,root,"sub"),SF_INVALID);
 int base=find("/cap"),parent=find("/cap/sub"),target=find("/cap/sub/nested/item");ns.nodes[base].readonly=1;REJECT(sf_ns_unlink(&ns,42,root,"sub/nested/item"),SF_READ_ONLY);ns.nodes[base].readonly=0;
 ns.nodes[parent].readonly=1;REJECT(sf_ns_unlink(&ns,1,global,"cap/sub/nested/item"),SF_READ_ONLY);ns.nodes[parent].readonly=0;
 ns.nodes[parent].mount_tag=7;REJECT(sf_ns_unlink(&ns,42,root,"sub/nested/item"),SF_READ_ONLY);ns.nodes[parent].mount_tag=0;
 ns.nodes[target].readonly=1;REJECT(sf_ns_unlink(&ns,42,root,"sub/nested/item"),SF_READ_ONLY);ns.nodes[target].readonly=0;ns.nodes[target].mount_tag=7;REJECT(sf_ns_unlink(&ns,42,root,"sub/nested/item"),SF_READ_ONLY);ns.nodes[target].mount_tag=0;
 ns.nodes[base].mount_tag=7;REJECT(sf_ns_unlink(&ns,42,root,"sub/nested/item"),SF_READ_ONLY);ns.nodes[base].mount_tag=0;CHECK(!sf_ns_unlink(&ns,42,root,"sub/nested/item"));
 CHECK(!sf_ns_create(&ns,"/cap/special",NODE_PIPE,0));REJECT(sf_ns_unlink(&ns,42,root,"special"),SF_INVALID);ns.nodes[find("/cap/special")].type=NODE_CONSOLE;REJECT(sf_ns_unlink(&ns,42,root,"special"),SF_INVALID);ns.nodes[find("/cap/special")].type=NODE_DISK;REJECT(sf_ns_unlink(&ns,42,root,"special"),SF_INVALID);
 CHECK(!sf_ns_create(&ns,"/cap/spaced file",NODE_FILE,0));CHECK(!sf_ns_unlink(&ns,42,root,"spaced file"));CHECK(!sf_ns_create(&ns,"/cap/\xe7\x8b\x90",NODE_FILE,0));CHECK(!sf_ns_unlink(&ns,42,root,"\xe7\x8b\x90"));
 char path[96]="/cap/";memset(path+5,'a',90);path[95]=0;CHECK(!sf_ns_create(&ns,path,NODE_FILE,0));CHECK(!sf_ns_unlink(&ns,42,root,path+5));
 /* No open description survives deletion. Recycled storage starts zeroed,
    while every retired handle still fails even after the same slot returns. */
 for(unsigned i=0;i<1000;i++){
  CHECK(!sf_ns_create(&ns,"/cap/item",NODE_FILE,0));int h=sf_ns_openat(&ns,42,root,"item",OPEN_READ|OPEN_WRITE);CHECK(h>0&&h!=file);CHECK(sf_ns_read(&ns,42,h,bytes,1)==0);CHECK(!sf_ns_truncate(&ns,42,h,sizeof(bytes)));memset(bytes,0xa5,sizeof(bytes));CHECK(sf_ns_read(&ns,42,h,bytes,sizeof(bytes))==sizeof(bytes));for(unsigned j=0;j<sizeof(bytes);j++)CHECK(!bytes[j]);CHECK(sf_ns_write(&ns,42,h,"secret",6)==6);CHECK(!sf_ns_close(&ns,42,h));CHECK(!sf_ns_unlink(&ns,42,root,"item"));CHECK(sf_ns_read(&ns,42,h,bytes,1)==SF_STALE_HANDLE);
 }
 CHECK(!sf_ns_create(&ns,"/cap/item",NODE_FILE,0));
 struct{uint8_t left[32];SfMessage m;uint8_t right[32];}g;
 uint32_t owned=root;unsigned lengths[]={0,257,0x80000000u,UINT32_MAX,4,12};
 for(unsigned i=0;i<sizeof(lengths)/sizeof(*lengths);i++){
  memset(&g,0xa5,sizeof(g));memset(&g.m,0,sizeof(g.m));g.m.h.opcode=OP_UNLINK;g.m.handle=root;g.m.length=lengths[i];memcpy(g.m.data,"item\0extra",11);if(lengths[i]==4)g.m.data[3]='m';saved=ns;
  CHECK(sf_file_dispatch(&ns,42,&owned,&g.m)==SF_INVALID);CHECK(g.m.status==SF_INVALID&&g.m.handle==(unsigned)root&&owned==(unsigned)root);CHECK(!memcmp(&ns,&saved,sizeof(ns)));for(unsigned j=0;j<32;j++)CHECK(g.left[j]==0xa5&&g.right[j]==0xa5);
 }
 memset(&g.m,0,sizeof(g.m));g.m.h.opcode=OP_UNLINK;g.m.handle=read;g.m.length=5;memcpy(g.m.data,"item",5);REJECT(sf_file_dispatch(&ns,43,&owned,&g.m),SF_DENIED);
 g.m.handle=root;CHECK(!sf_file_dispatch(&ns,42,&owned,&g.m));CHECK(g.m.handle==(unsigned)root&&owned==(unsigned)root&&find("/cap/item")<0);g.m.h.opcode=OP_ROOT;CHECK(!sf_file_dispatch(&ns,42,&owned,&g.m)&&g.m.handle==owned);
 CHECK(!sf_ns_close(&ns,42,root));REJECT(sf_ns_unlink(&ns,42,root,"item"),SF_STALE_HANDLE);sf_ns_reap(&ns,1);sf_ns_reap(&ns,43);for(unsigned i=0;i<32;i++)CHECK(!ns.descriptions[i].refs);
 printf("UNLINK PASS: %u assertions\n",checks);return 0;
}
