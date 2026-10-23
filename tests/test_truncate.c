#include <stdio.h>
#include "namespace.h"
static SfNamespace ns;static unsigned checks,calls;static int failure;static uint8_t disk[16384];static unsigned disk_size;
#define CHECK(x) do{checks++;if(!(x)){printf("TRUNCATE FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int resize(unsigned index,uint64_t size){calls++;if(index!=7)return SF_INVALID;if(failure)return failure;if(size>sizeof(disk))return SF_NO_SPACE;unsigned start=size<disk_size?size:disk_size;memset(disk+start,0,sizeof(disk)-start);disk_size=size;return 0;}
static long read_disk(unsigned index,uint64_t offset,void*out,size_t size){if(index!=7)return SF_INVALID;if(offset>=disk_size)return 0;if(size>disk_size-offset)size=disk_size-offset;memcpy(out,disk+offset,size);return size;}
int main(void){
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/file",NODE_FILE,0));CHECK(!sf_ns_create(&ns,"/pipe",NODE_PIPE,0));CHECK(!sf_ns_create(&ns,"/console",NODE_CONSOLE,0));CHECK(!sf_ns_create(&ns,"/disk",NODE_DISK,0));CHECK(!sf_ns_create(&ns,"/rom",NODE_FILE,1));
 int h=sf_ns_open(&ns,42,"/","/file",3),r=sf_ns_open(&ns,42,"/","/file",OPEN_READ);CHECK(h>0&&r>0);CHECK(sf_ns_write(&ns,42,h,"abcdef",6)==6);int dup=sf_ns_dup(&ns,42,h);CHECK(dup>0);
 CHECK(sf_ns_truncate(&ns,43,h,0)==SF_STALE_HANDLE);CHECK(sf_ns_truncate(&ns,42,h^256,0)==SF_STALE_HANDLE);CHECK(sf_ns_truncate(&ns,42,r,0)==SF_DENIED);
 CHECK(!sf_ns_truncate(&ns,42,h,2));uint8_t out[1024];CHECK(sf_ns_read(&ns,42,r,out,sizeof(out))==2&&!memcmp(out,"ab",2));CHECK(sf_ns_read(&ns,42,dup,out,1)==0);
 CHECK(sf_ns_write(&ns,42,dup,"X",1)==1);CHECK(sf_ns_seek(&ns,42,h,0)==0);CHECK(sf_ns_read(&ns,42,h,out,sizeof(out))==7&&out[0]=='a'&&out[1]=='b'&&out[6]=='X');for(unsigned i=2;i<6;i++)CHECK(out[i]==0);
 CHECK(!sf_ns_truncate(&ns,42,h,6));CHECK(!sf_ns_truncate(&ns,42,h,10));CHECK(sf_ns_seek(&ns,42,r,0)==0);CHECK(sf_ns_read(&ns,42,r,out,sizeof(out))==10);for(unsigned i=2;i<10;i++)CHECK(out[i]==0);
 CHECK(!sf_ns_truncate(&ns,42,h,0));CHECK(sf_ns_read(&ns,42,r,out,1)==0);CHECK(sf_ns_write(&ns,42,h,"Y",1)==1);CHECK(sf_ns_seek(&ns,42,r,0)==0);CHECK(sf_ns_read(&ns,42,r,out,sizeof(out))==8&&out[7]=='Y');for(unsigned i=0;i<7;i++)CHECK(out[i]==0);
 CHECK(!sf_ns_truncate(&ns,42,h,1024));CHECK(sf_ns_truncate(&ns,42,h,1025)==SF_NO_SPACE);CHECK(sf_ns_truncate(&ns,42,h,UINT64_MAX)==SF_NO_SPACE);CHECK(sf_ns_seek(&ns,42,r,1023)==1023&&sf_ns_read(&ns,42,r,out,1)==1&&out[0]==0);
 const char*special[]={"/","/pipe","/console"};for(unsigned i=0;i<3;i++){int s=sf_ns_open(&ns,42,"/",special[i],OPEN_WRITE);CHECK(s>0);CHECK(sf_ns_truncate(&ns,42,s,0)==SF_INVALID);CHECK(!sf_ns_close(&ns,42,s));}
 int ro=sf_ns_open(&ns,42,"/","/rom",OPEN_READ);CHECK(ro>0&&sf_ns_truncate(&ns,42,ro,0)==SF_DENIED);
 unsigned node=0;for(unsigned i=0;i<32;i++)if(!strcmp(ns.nodes[i].path,"/disk"))node=i;ns.nodes[node].external=7;
 int d=sf_ns_open(&ns,42,"/","/disk",3);CHECK(d>0);CHECK(sf_ns_truncate(&ns,42,d,3)==SF_READ_ONLY);ns.external_truncate=resize;ns.external_read=read_disk;
 memcpy(disk,"abcdef",6);disk_size=ns.nodes[node].size=6;CHECK(sf_ns_seek(&ns,42,d,6)==6);CHECK(!sf_ns_truncate(&ns,42,d,2)&&ns.nodes[node].size==2);CHECK(sf_ns_read(&ns,42,d,out,1)==0);
 CHECK(!sf_ns_truncate(&ns,42,d,10)&&ns.nodes[node].size==10);CHECK(sf_ns_seek(&ns,42,d,0)==0);CHECK(sf_ns_read(&ns,42,d,out,10)==10&&!memcmp(out,"ab",2));for(unsigned i=2;i<10;i++)CHECK(out[i]==0);
 failure=SF_COMMIT_UNCERTAIN;unsigned before=calls;CHECK(sf_ns_truncate(&ns,42,d,1)==SF_COMMIT_UNCERTAIN&&calls==before+1&&ns.nodes[node].size==10);CHECK(sf_ns_seek(&ns,42,d,10)==10);
 failure=1;CHECK(sf_ns_truncate(&ns,42,d,1)==SF_BAD_FORMAT&&ns.nodes[node].size==10);failure=SF_DEVICE_GONE;CHECK(sf_ns_truncate(&ns,42,d,0)==SF_DEVICE_GONE&&ns.nodes[node].size==10);failure=0;
 before=calls;CHECK(sf_ns_truncate(&ns,42,d,UINT64_MAX)==SF_NO_SPACE&&calls==before);CHECK(sf_ns_truncate(&ns,42,d,16385)==SF_NO_SPACE&&ns.nodes[node].size==10);ns.nodes[node].readonly=1;before=calls;CHECK(sf_ns_truncate(&ns,42,d,0)==SF_READ_ONLY&&calls==before);
 sf_ns_reap(&ns,42);CHECK(sf_ns_truncate(&ns,42,h,0)==SF_STALE_HANDLE);printf("TRUNCATE PASS: %u namespace assertions\n",checks);return 0;
}
