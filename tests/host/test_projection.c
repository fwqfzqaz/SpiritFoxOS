#include <stdio.h>
#include "namespace.h"
#include "../../services/namespace_state.h"
static SfNamespace ns;static unsigned checks,calls;static uint8_t payload[64];static unsigned payload_size;static unsigned last_id;
#define CHECK(x) do{checks++;if(!(x)){printf("PROJECTION FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static long provide(unsigned id,uint64_t offset,void*out,size_t bytes){calls++;last_id=id;if(id!=7)return SF_INVALID;if(offset>=payload_size)return 0;if(bytes>payload_size-offset)bytes=payload_size-offset;memcpy(out,payload+offset,bytes);return(long)bytes;}
int main(void){
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/proc",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/proc/self",NODE_DIR,1));
 CHECK(!sf_ns_project(&ns,"/proc/uptime",7));CHECK(!sf_ns_project(&ns,"/proc/ipc",8));
 CHECK(sf_ns_project(&ns,"/proc/uptime",7)==SF_BUSY);CHECK(sf_ns_project(&ns,"/proc/zero",0)==SF_INVALID);CHECK(sf_ns_project(&ns,"/missing/x",9)==SF_NOT_FOUND);
 CHECK(sf_ns_project(&ns,"/proc/self/status",7)==SF_BUSY);
 int h=sf_ns_open(&ns,1,"/","/proc/uptime",OPEN_READ);CHECK(h>0);uint8_t out[64];
 CHECK(sf_ns_read(&ns,1,h,out,4)==SF_DEVICE_GONE);CHECK(sf_ns_write(&ns,1,h,out,1)==SF_DENIED);CHECK(sf_ns_open(&ns,1,"/","/proc/uptime",OPEN_WRITE)==SF_READ_ONLY);
 ns.projection_read=provide;memcpy(payload,"abcdefghijklm",13);payload_size=13;
 CHECK(sf_ns_read(&ns,1,h,out,3)==3&&!memcmp(out,"abc",3));CHECK(sf_ns_read(&ns,1,h,out,sizeof(out))==10&&!memcmp(out,"defghijklm",10));
 CHECK(sf_ns_read(&ns,1,h,out,4)==0);CHECK(calls==3&&last_id==7);
 CHECK(sf_ns_pread(&ns,1,h,out,2,5)==2&&!memcmp(out,"fg",2));CHECK(sf_ns_pread(&ns,1,h,out,64,13)==0);
 CHECK(sf_ns_seek(&ns,1,h,4)==4&&sf_ns_read(&ns,1,h,out,2)==2&&!memcmp(out,"ef",2));
 CHECK(sf_ns_seek(&ns,1,h,100)==100&&sf_ns_read(&ns,1,h,out,1)==0);
 SfFileStat stat;CHECK(!sf_ns_stat(&ns,1,h,&stat)&&stat.type==NODE_PROJECTION&&stat.flags==SF_FILE_READ_ONLY&&stat.size==0&&stat.mode==OPEN_READ&&stat.offset==100);
 CHECK((sf_ns_poll(&ns,1,h)&1)&&!(sf_ns_poll(&ns,1,h)&2));CHECK(sf_ns_truncate(&ns,1,h,0)==SF_DENIED);
 unsigned node=0;for(unsigned i=0;i<SF_NS_NODE_MAX;i++)if(!strcmp(ns.nodes[i].path,"/proc/uptime"))node=i;
 CHECK(node<SF_NS_NODE_MAX&&ns.nodes[node].type==NODE_PROJECTION&&ns.nodes[node].size==0&&ns.nodes[node].readonly==1&&!ns.nodes[node].data[0]);
 int root=sf_ns_open(&ns,1,"/","/proc",OPEN_READ|OPEN_WRITE);CHECK(root>0);CHECK(sf_ns_unlink(&ns,1,root,"uptime")==SF_READ_ONLY);CHECK(sf_ns_rmdir(&ns,1,root,"self")==SF_READ_ONLY);
 CHECK(!sf_ns_close(&ns,1,root));CHECK(!sf_ns_protect(&ns,"/proc"));CHECK(sf_ns_create(&ns,"/proc/new",NODE_FILE,0)==SF_READ_ONLY);
 CHECK(!sf_ns_close(&ns,1,h));
 unsigned filled=0;for(unsigned i=0;i<SF_NS_NODE_MAX;i++){char path[24];snprintf(path,sizeof(path),"/fill%u",i);int r=sf_ns_create(&ns,path,NODE_FILE,0);if(r==SF_NO_SPACE)break;CHECK(!r);filled++;}
 CHECK(filled>0);
 printf("PROJECTION PASS: %u namespace assertions\n",checks);return 0;
}
