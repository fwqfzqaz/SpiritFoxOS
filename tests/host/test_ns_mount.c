#include <stdio.h>
#include "namespace.h"

#include "../../services/namespace_state.h"
static SfNamespace ns,before;
static unsigned checks,leases,admission=1;
#define CHECK(x) do{checks++;if(!(x)){printf("NS MOUNT FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int acquire(unsigned tag){if(tag!=7)return SF_INVALID;if(!admission)return SF_DRAINING;leases++;return 0;}
static void release(unsigned tag){if(tag==7)leases--;}
static long read_disk(unsigned id,uint64_t offset,void*out,size_t bytes){if(id!=3)return SF_INVALID;if(offset>=8)return 0;if(bytes>8-offset)bytes=8-offset;memcpy(out,&"abcdefgh"[offset],bytes);return (long)bytes;}
static long read_notes(unsigned id,uint64_t offset,void*out,size_t bytes){if(id!=UINT32_MAX)return read_disk(id,offset,out,bytes);if(offset>=3)return 0;if(bytes>3-offset)bytes=3-offset;memcpy(out,&"new"[offset],bytes);return (long)bytes;}
static const SfMountEntry disk[]={
 {"/disk",NODE_DIR,1,0,0},
 {"/disk/sub",NODE_DIR,1,0,0},
 {"/disk/sub/file",NODE_DISK,1,3,8}
};
static const SfMountEntry persist[]={
 {"/persist",NODE_DIR,1,0,0},
 {"/persist/notes",NODE_DISK,0,UINT32_MAX,3}
};
static int unchanged(const SfMountEntry*entries,unsigned count,unsigned tag,int error){
 before=ns;CHECK(sf_ns_mount_publish(&ns,entries,count,tag)==error);CHECK(!memcmp(&before,&ns,sizeof(ns)));return 0;
}
int main(void){
 sf_ns_init(&ns,0);
 CHECK(!sf_ns_mount_check(&ns,"/disk"));
 CHECK(sf_ns_mount_check(&ns,"/")==SF_INVALID);
 CHECK(sf_ns_mount_check(&ns,"/missing/child")==SF_NOT_FOUND);
 CHECK(!unchanged(0,1,7,SF_INVALID));CHECK(!unchanged(disk,0,7,SF_INVALID));
 CHECK(!unchanged(disk,SF_NS_NODE_MAX+1,7,SF_INVALID));CHECK(!unchanged(disk,3,0,SF_INVALID));
 SfMountEntry bad[3];memcpy(bad,disk,sizeof(bad));
 memset(bad[2].path,'x',sizeof(bad[2].path));CHECK(!unchanged(bad,3,7,SF_INVALID));
 const char*invalid[]={"/disk/../escape","/disk//file","/disk/sub/","/disk/\xc0\xaf","disk/file"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){memcpy(bad,disk,sizeof(bad));memset(bad[2].path,0,96);memcpy(bad[2].path,invalid[i],strlen(invalid[i])+1);CHECK(!unchanged(bad,3,7,SF_INVALID));}
 memcpy(bad,disk,sizeof(bad));memcpy(bad[2].path,"/disk2/file",12);CHECK(!unchanged(bad,3,7,SF_DENIED));
 memcpy(bad,disk,sizeof(bad));bad[0].type=NODE_DISK;CHECK(!unchanged(bad,3,7,SF_INVALID));
 memcpy(bad,disk,sizeof(bad));bad[1].type=NODE_FILE;CHECK(!unchanged(bad,3,7,SF_INVALID));
 memcpy(bad,disk,sizeof(bad));bad[2].readonly=2;CHECK(!unchanged(bad,3,7,SF_INVALID));
 memcpy(bad,disk,sizeof(bad));bad[2]=bad[1];CHECK(!unchanged(bad,3,7,SF_BUSY));
 memcpy(bad,disk,sizeof(bad));SfMountEntry swap=bad[1];bad[1]=bad[2];bad[2]=swap;CHECK(!unchanged(bad,3,7,SF_NOT_FOUND));
 memcpy(bad,disk,sizeof(bad));bad[1].type=NODE_DISK;CHECK(!unchanged(bad,3,7,SF_NOT_FOUND));
 CHECK(!sf_ns_create(&ns,"/disk",NODE_FILE,0));CHECK(!unchanged(disk,3,7,SF_BUSY));
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/disk",NODE_DIR,0));
 CHECK(!sf_ns_create(&ns,"/disk/local",NODE_FILE,0));CHECK(!unchanged(disk,3,7,SF_BUSY));
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/disk",NODE_DIR,0));
 CHECK(!sf_ns_assign(&ns,"/disk",17,23,0750));
 int root=sf_ns_open(&ns,41,"/","/disk",OPEN_READ|OPEN_DIRECTORY);CHECK(root>0);
 CHECK(!unchanged(disk,3,7,SF_BUSY));CHECK(!sf_ns_close(&ns,41,root));
 CHECK(sf_ns_external(&ns,read_disk,0,0)==0);sf_ns_mount_hooks(&ns,acquire,release);
 CHECK(!sf_ns_mount_publish(&ns,disk,3,7));CHECK(leases==0);
 CHECK(sf_ns_mount_check(&ns,"/disk")==SF_BUSY);
 CHECK(sf_ns_open_as(&ns,42,"/","/disk/sub/file",OPEN_READ,99,99)==SF_DENIED);
 int file=sf_ns_open_as(&ns,42,"/","/disk/sub/file",OPEN_READ,17,23);CHECK(file>0&&leases==1);
 SfFileStat stat;CHECK(!sf_ns_stat(&ns,42,file,&stat));CHECK(stat.size==8&&(stat.flags&SF_FILE_READ_ONLY)&&(stat.flags&SF_FILE_MOUNTED));
 CHECK(sf_ns_create(&ns,"/disk/new",NODE_FILE,0)==SF_READ_ONLY);
 CHECK(sf_ns_open(&ns,42,"/","/disk/sub/file",OPEN_WRITE)==SF_READ_ONLY);
 char data[8]={0};CHECK(sf_ns_read(&ns,42,file,data,2)==2&&!memcmp(data,"ab",2));
 int duplicate=sf_ns_dup(&ns,42,file);CHECK(duplicate>0&&leases==1);
 CHECK(sf_ns_handoff(&ns,0,file,43)==SF_INVALID);CHECK(sf_ns_handoff(&ns,42,file,0)==SF_INVALID);
 CHECK(sf_ns_handoff(&ns,44,file,43)==SF_STALE_HANDLE);CHECK(sf_ns_handoff(&ns,42,file^256,43)==SF_STALE_HANDLE);
 CHECK(sf_ns_handoff(&ns,42,0,43)==SF_STALE_HANDLE);CHECK(sf_ns_handoff(&ns,42,255,43)==SF_STALE_HANDLE);
 CHECK(!sf_ns_handoff(&ns,42,file,43));CHECK(leases==1);
 CHECK(sf_ns_read(&ns,42,file,data,1)==SF_STALE_HANDLE);CHECK(sf_ns_read(&ns,43,duplicate,data,1)==SF_STALE_HANDLE);
 CHECK(sf_ns_read(&ns,43,file,data,2)==2&&!memcmp(data,"cd",2));
 CHECK(sf_ns_read(&ns,42,duplicate,data,2)==2&&!memcmp(data,"ef",2));
 sf_ns_reap(&ns,42);CHECK(leases==1);CHECK(sf_ns_unmount(&ns,7)==SF_BUSY);
 CHECK(sf_ns_read(&ns,43,file,data,2)==2&&!memcmp(data,"gh",2));
 CHECK(!sf_ns_handoff(&ns,43,file,43));
 sf_ns_reap(&ns,43);CHECK(leases==0);CHECK(sf_ns_handoff(&ns,43,file,44)==SF_STALE_HANDLE);
 CHECK(!sf_ns_unmount(&ns,7));
 CHECK(sf_ns_read(&ns,43,file,data,1)==SF_STALE_HANDLE);
 CHECK(!sf_ns_mount_publish(&ns,disk,3,7));admission=0;
 CHECK(sf_ns_open(&ns,42,"/","/disk",OPEN_READ)==SF_DRAINING);CHECK(leases==0);admission=1;
 CHECK(!sf_ns_mount_publish(&ns,persist,2,7));
 const SfMountEntry rejected[]={{"/other",NODE_DIR,1,0,0},{"/escape",NODE_DISK,1,3,8}};
 CHECK(!unchanged(rejected,2,7,SF_DENIED));
 CHECK(sf_ns_external(&ns,read_notes,0,0)==read_disk);
 int note=sf_ns_open(&ns,44,"/","/persist/notes",OPEN_READ);CHECK(note>0&&leases==1);
 CHECK(sf_ns_read(&ns,44,note,data,8)==3&&!memcmp(data,"new",3));CHECK(!sf_ns_close(&ns,44,note));
 int disk_file=sf_ns_open(&ns,44,"/","/disk/sub/file",OPEN_READ);CHECK(disk_file>0);
 CHECK(sf_ns_read(&ns,44,disk_file,data,8)==8&&!memcmp(data,"abcdefgh",8));CHECK(!sf_ns_close(&ns,44,disk_file));
 CHECK(sf_ns_external(&ns,read_notes,0,0)==read_notes);
 CHECK(sf_ns_unmount(&ns,7)==0&&leases==0);
 /* Near-capacity failure must not consume the last free node or erase an
  * existing empty root. Fill all but one slot; two child nodes cannot fit. */
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/disk",NODE_DIR,0));
 for(unsigned i=0;i<SF_NS_NODE_MAX-3;i++){char name[32];snprintf(name,sizeof(name),"/f%u",i);CHECK(!sf_ns_create(&ns,name,NODE_FILE,0));}
 CHECK(!unchanged(disk,3,7,SF_NO_SPACE));CHECK(!sf_ns_create(&ns,"/last",NODE_FILE,0));
 CHECK(!sf_ns_mount_publish(&ns,disk,1,7));
 CHECK(!unchanged(disk,1,8,SF_BUSY));
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/locked",NODE_DIR,1));
 const SfMountEntry locked[]={{"/locked/disk",NODE_DIR,1,0,0}};
 CHECK(!unchanged(locked,1,7,SF_READ_ONLY));
 printf("Namespace mount/handoff tests passed: %u assertions\n",checks);return 0;
}
