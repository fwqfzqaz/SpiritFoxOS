#include <stdio.h>
#include "namespace.h"
static SfNamespace ns;static unsigned checks,leases,admission=1;
#define CHECK(x) do{checks++;if(!(x)){printf("OPENAT FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int mount_open(unsigned tag){if(tag!=7)return SF_INVALID;if(!admission)return SF_DRAINING;leases++;return 0;}
static void mount_close(unsigned tag){if(tag==7)leases--;}
int main(void){
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/cap",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/cap/sub",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/cap/sub/data",NODE_FILE,0));CHECK(!sf_ns_create(&ns,"/outside",NODE_FILE,0));
 int writer=sf_ns_open(&ns,1,"/","/cap/sub/data",OPEN_WRITE);CHECK(writer>0);CHECK(sf_ns_write(&ns,1,writer,"abcdef",6)==6);CHECK(!sf_ns_close(&ns,1,writer));
 int root=sf_ns_open(&ns,42,"/","/cap",OPEN_READ|OPEN_DIRECTORY);CHECK(root>0);
 CHECK(sf_ns_open(&ns,42,"/","/cap/sub/data",OPEN_READ|OPEN_DIRECTORY)==SF_INVALID);CHECK(sf_ns_open(&ns,42,"/outside","/outside",OPEN_READ)==SF_INVALID);
 int file=sf_ns_openat(&ns,42,root,"sub/data",OPEN_READ);CHECK(file>0);char out[16]={0};CHECK(sf_ns_read(&ns,42,file,out,2)==2&&!memcmp(out,"ab",2));
 int second=sf_ns_openat(&ns,42,root,"sub/data",OPEN_READ);CHECK(second>0);CHECK(sf_ns_read(&ns,42,second,out,2)==2&&!memcmp(out,"ab",2));
 int duplicate=sf_ns_dup(&ns,42,file);CHECK(duplicate>0);CHECK(sf_ns_read(&ns,42,duplicate,out,2)==2&&!memcmp(out,"cd",2));
 CHECK(sf_ns_openat(&ns,43,root,"sub/data",OPEN_READ)==SF_STALE_HANDLE);CHECK(sf_ns_openat(&ns,42,root^256,"sub/data",OPEN_READ)==SF_STALE_HANDLE);
 CHECK(sf_ns_openat(&ns,42,root,"sub/data",OPEN_WRITE)==SF_DENIED);CHECK(sf_ns_openat(&ns,42,root,"sub",OPEN_WRITE|OPEN_DIRECTORY)==SF_DENIED);CHECK(sf_ns_write(&ns,42,file,"x",1)==SF_DENIED);
 CHECK(sf_ns_openat(&ns,42,file,"data",OPEN_READ)==SF_INVALID);CHECK(sf_ns_openat(&ns,42,root,"missing",OPEN_READ)==SF_NOT_FOUND);
 const char*bad[]={"","/outside","..","../outside","sub/../../outside",".","sub/./data","sub//data","sub/data/","sub/..","sub/", "\xc0\xafoutside","\xe7","sub/\x01"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)CHECK(sf_ns_openat(&ns,42,root,bad[i],OPEN_READ)==SF_INVALID);
 CHECK(sf_ns_openat(&ns,42,root,"sub/data",0)==SF_INVALID);CHECK(sf_ns_openat(&ns,42,root,"sub/data",16|OPEN_READ)==SF_INVALID);
 int sub=sf_ns_openat(&ns,42,root,"sub",OPEN_READ|OPEN_DIRECTORY);CHECK(sub>0);CHECK(sf_ns_openat(&ns,42,sub,"../sub/data",OPEN_READ)==SF_INVALID);CHECK(sf_ns_openat(&ns,42,sub,"data",OPEN_READ)>0);
 CHECK(!sf_ns_create(&ns,"/cap/\xe7\x8b\x90",NODE_FILE,0));CHECK(sf_ns_openat(&ns,42,root,"\xe7\x8b\x90",OPEN_READ)>0);
 CHECK(!sf_ns_create(&ns,"/cap/..x",NODE_FILE,0));CHECK(sf_ns_openat(&ns,42,root,"..x",OPEN_READ)>0);CHECK(sf_ns_create(&ns,"/cap/alias/",NODE_DIR,0)==SF_INVALID);
 char path[97]="/cap/",relative[92];memset(relative,'a',90);relative[90]=0;memcpy(path+5,relative,91);CHECK(!sf_ns_create(&ns,path,NODE_FILE,0));CHECK(sf_ns_openat(&ns,42,root,relative,OPEN_READ)>0);relative[90]='b';relative[91]=0;CHECK(sf_ns_openat(&ns,42,root,relative,OPEN_READ)==SF_INVALID);
 int global=sf_ns_open(&ns,7,"/","/",OPEN_READ|OPEN_DIRECTORY);CHECK(global>0);CHECK(sf_ns_openat(&ns,7,global,"outside",OPEN_READ)>0);
 CHECK(!sf_ns_close(&ns,42,root));CHECK(sf_ns_read(&ns,42,file,out,2)==2&&!memcmp(out,"ef",2));CHECK(sf_ns_openat(&ns,42,root,"sub/data",OPEN_READ)==SF_STALE_HANDLE);
 sf_ns_reap(&ns,42);CHECK(sf_ns_read(&ns,42,file,out,1)==SF_STALE_HANDLE);CHECK(sf_ns_openat(&ns,42,sub,"data",OPEN_READ)==SF_STALE_HANDLE);
 sf_ns_reap(&ns,7);
 /* A write-capable trusted directory can attenuate to read-only descendants. */
 root=sf_ns_open(&ns,42,"/","/cap",OPEN_READ|OPEN_WRITE|OPEN_DIRECTORY);CHECK(root>0);sub=sf_ns_openat(&ns,42,root,"sub",OPEN_READ|OPEN_DIRECTORY);CHECK(sub>0);CHECK(sf_ns_openat(&ns,42,sub,"data",OPEN_WRITE)==SF_DENIED);writer=sf_ns_openat(&ns,42,root,"sub/data",OPEN_WRITE);CHECK(writer>0);CHECK(sf_ns_read(&ns,42,writer,out,1)==SF_DENIED);sf_ns_reap(&ns,42);
 /* Parent and child hold independent mount leases. Admission failures and
    capacity exhaustion must never leak a lease. */
 ns.mount_open=mount_open;ns.mount_close=mount_close;for(unsigned i=0;i<32;i++)if(ns.nodes[i].type&&(!strcmp(ns.nodes[i].path,"/cap")||!strcmp(ns.nodes[i].path,"/cap/sub")||!strcmp(ns.nodes[i].path,"/cap/sub/data")))ns.nodes[i].mount_tag=7;
 root=sf_ns_open(&ns,42,"/","/cap",OPEN_READ|OPEN_DIRECTORY);CHECK(root>0&&leases==1);file=sf_ns_openat(&ns,42,root,"sub/data",OPEN_READ);CHECK(file>0&&leases==2);CHECK(sf_ns_unmount(&ns,7)==SF_BUSY);
 admission=0;CHECK(sf_ns_openat(&ns,42,root,"sub/data",OPEN_READ)==SF_DRAINING&&leases==2);admission=1;
 CHECK(!sf_ns_close(&ns,42,root)&&leases==1);CHECK(sf_ns_unmount(&ns,7)==SF_BUSY);CHECK(!sf_ns_close(&ns,42,file)&&leases==0);
 root=sf_ns_open(&ns,42,"/","/cap",OPEN_READ|OPEN_DIRECTORY);CHECK(root>0);for(unsigned i=0;i<31;i++)CHECK(sf_ns_openat(&ns,42,root,"sub/data",OPEN_READ)>0);CHECK(leases==32);CHECK(sf_ns_openat(&ns,42,root,"sub/data",OPEN_READ)==SF_NO_SPACE&&leases==32);sf_ns_reap(&ns,42);CHECK(leases==0);
 for(unsigned i=0;i<64;i++)ns.handles[i].generation=0x7fffff;CHECK(sf_ns_open(&ns,42,"/","/cap",OPEN_READ)==SF_NO_SPACE&&leases==0);ns.handles[63].generation=0;root=sf_ns_open(&ns,42,"/","/cap",OPEN_READ|OPEN_DIRECTORY);CHECK(root>0&&(root&255)==64&&leases==1);CHECK(!sf_ns_close(&ns,42,root)&&leases==0);CHECK(!sf_ns_unmount(&ns,7));
 printf("OPENAT PASS: %u assertions\n",checks);return 0;
}
