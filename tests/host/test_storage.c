#include <stdio.h>
#include "namespace.h"
#include "virtio.h"
static SfNamespace ns;static unsigned leases,count;static int admission=1;
#define CHECK(x) do{count++;if(!(x)){printf("FAIL storage line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int mount_open(unsigned tag){if(tag!=1)return SF_INVALID;if(!admission)return SF_DRAINING;leases++;return 0;}
static void mount_close(unsigned tag){if(tag==1)leases--;}
static long external(unsigned index,uint64_t offset,void*out,size_t bytes){if(index!=7)return SF_INVALID;if(offset>=4096)return 0;if(bytes>4096-offset)bytes=4096-offset;memset(out,'a'+offset%26,bytes);return bytes;}
int main(void){
 uint32_t used;CHECK(!sf_virtq_layout(256,&used)&&used==8192);CHECK(!sf_virtq_layout(8,&used)&&used==4096);CHECK(sf_virtq_layout(0,&used)==SF_BAD_FORMAT);CHECK(sf_virtq_layout(3,&used)==SF_BAD_FORMAT);CHECK(sf_virtq_layout(512,&used)==SF_BAD_FORMAT);
 CHECK(sf_block_range(2048,0,0,256));CHECK(sf_block_range(2048,2047*512,256,256));CHECK(!sf_block_range(2048,2048*512,0,256));CHECK(!sf_block_range(2048,1,0,256));CHECK(!sf_block_range(2048,0,512,256));CHECK(!sf_block_range(2048,0,1,256));CHECK(!sf_block_range(2048,0,0,257));CHECK(!sf_block_range(2048,UINT64_MAX,0,256));
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/disk",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/disk/item",NODE_DISK,1));ns.external_read=external;ns.mount_open=mount_open;ns.mount_close=mount_close;
 for(unsigned i=0;i<32;i++)if(ns.nodes[i].type&&!strcmp(ns.nodes[i].path,"/disk")){ns.nodes[i].mount_tag=1;ns.nodes[i].readonly=1;}else if(ns.nodes[i].type==NODE_DISK){ns.nodes[i].mount_tag=1;ns.nodes[i].external=7;ns.nodes[i].size=4096;}
 int h=sf_ns_open(&ns,42,"/","/disk/item",1);CHECK(h>0&&leases==1);int dup=sf_ns_dup(&ns,42,h);CHECK(dup>0&&leases==1);CHECK(sf_ns_unmount(&ns,1)==SF_BUSY);CHECK(sf_ns_open(&ns,42,"/","/disk/item",2)==SF_READ_ONLY);CHECK(sf_ns_create(&ns,"/disk/new",NODE_FILE,0)==SF_READ_ONLY);
 char data[2048];CHECK(sf_ns_seek(&ns,42,h,2000)==2000);CHECK(sf_ns_read(&ns,42,dup,data,sizeof(data))==2048);CHECK(sf_ns_read(&ns,42,h,data,sizeof(data))==48);CHECK(!sf_ns_read(&ns,42,h,data,1));CHECK(sf_ns_seek(&ns,42,h,4097)==SF_INVALID);
 admission=0;CHECK(sf_ns_open(&ns,42,"/","/disk/item",1)==SF_DRAINING);CHECK(leases==1);CHECK(!sf_ns_close(&ns,42,h)&&leases==1);CHECK(!sf_ns_close(&ns,42,dup)&&leases==0);admission=1;
 int dir=sf_ns_open(&ns,42,"/","/disk",1);CHECK(dir>0&&leases==1);CHECK(sf_ns_unmount(&ns,1)==SF_BUSY);sf_ns_reap(&ns,42);CHECK(leases==0);CHECK(!sf_ns_unmount(&ns,1));CHECK(sf_ns_open(&ns,42,"/","/disk/item",1)==SF_NOT_FOUND);CHECK(sf_ns_read(&ns,42,h,data,1)==SF_STALE_HANDLE);
 printf("Storage service tests passed: %u assertions\n",count);return 0;
}
