/* SFRWFS1 directory mutations through the namespace frontend/runtime. The
 * volume is a real formatted image over a memory device so create, remove,
 * rename, permissions and remount visibility all exercise production code. */
#include <stdio.h>
#include "namespace_runtime.h"
#include "namespace_client.h"
#include "sfrw.h"
#define BLOCKS 160u
static uint8_t store[BLOCKS*SF_RW_BLOCK_BYTES];
static SfReadFs fs;
static SfNsClient client;
static Sfrw rw;
static unsigned checks,leases;
#define CHECK(x) do{checks++;if(!(x)){printf("NS RW FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
/* This scenario never mounts a readfs volume; the runtime still references the
 * parser entry point, so fail it explicitly instead of pulling the parser in. */
long sf_readfs_read(SfReadFs*f,unsigned index,uint64_t offset,void*out,size_t bytes){
 (void)f;(void)index;(void)offset;(void)out;(void)bytes;return SF_INVALID;
}
static int dev_read(void*context,uint32_t block,uint8_t*out){
 (void)context;if(!out||block>=BLOCKS)return SF_INVALID;
 memcpy(out,store+block*SF_RW_BLOCK_BYTES,SF_RW_BLOCK_BYTES);return 0;
}
static int dev_write(void*context,uint32_t block,const uint8_t*in){
 (void)context;if(!in||block>=BLOCKS)return SF_INVALID;
 memcpy(store+block*SF_RW_BLOCK_BYTES,in,SF_RW_BLOCK_BYTES);return 0;
}
static int dev_flush(void*context){(void)context;return 0;}
static SfrwDev device(void){SfrwDev d={dev_read,dev_write,dev_flush,0};return d;}
static int64_t root_reply(void*context,SfMessage*m){
 (void)context;m->length=0;m->status=0;
 switch(m->h.opcode){
 case OP_NS_CB_STORAGE_LIVE:m->status=0;break;
 case OP_NS_CB_MOUNT_OPEN:if(m->handle!=1)m->status=SF_STALE_HANDLE;else leases++;break;
 case OP_NS_CB_MOUNT_CLOSE:if(leases)leases--;break;
 default:m->status=SF_INVALID;break;
 }
 m->h.flags=1;m->h.deadline_ns=0;return 1;
}
static int64_t exchange(void*context,SfMessage*m){
 (void)context;m->h.flags=1;sf_nsv_dispatch(1,m);if(sf_nsv_faulted())return SF_DEVICE_GONE;
 m->h.flags=77;m->h.deadline_ns=0;return 77;
}
static int disk_has(uint32_t inode,const char*text,size_t bytes){
 char out[16];if(sf_rw_read(&rw,inode,0,0,out,bytes)!=(long)bytes)return 0;
 return !memcmp(out,text,bytes);
}
int main(void){
 memset(store,0,sizeof(store));
 SfrwFormatOptions options={BLOCKS,0,0};
 SfrwDev dev=device();
 CHECK(!sf_rw_format(&rw,dev,&options));
 CHECK(!sf_rw_mount(&rw,dev));
 sf_nsv_init(&fs,root_reply,0);
 CHECK(!sf_nsc_connect(&client,77,exchange,0,0,SF_NS_B_ALL));
 CHECK(!sf_nsv_attach_rw(&rw,1));
 /* mkdir + file create are mirrored to the formatted volume immediately. */
 CHECK(!sf_nsc_create(&client,"/disk/docs",NODE_DIR,0,0,0,0755));
 CHECK(!sf_nsc_create(&client,"/disk/docs/note.txt",NODE_FILE,0,0,0,0644));
 int file=sf_nsc_open(&client,42,"/","/disk/docs/note.txt",OPEN_READ|OPEN_WRITE,0,0);
 CHECK(file>0&&leases==1&&sf_nsc_write(&client,42,(uint32_t)file,"hello",5)==5&&!sf_nsc_close(&client,42,(uint32_t)file));
 uint32_t ino=0;
 CHECK(!sf_rw_open(&rw,"/docs/note.txt",0,SF_RW_READ,&ino)&&disk_has(ino,"hello",5));
 /* A reopened namespace handle reads the on-disk bytes, not a RAM copy. */
 file=sf_nsc_open(&client,42,"/","/disk/docs/note.txt",OPEN_READ,0,0);
 char out[8]={0};
 CHECK(file>0&&sf_nsc_read(&client,42,(uint32_t)file,out,5,0,0)==5&&!memcmp(out,"hello",5));
 CHECK(!sf_nsc_close(&client,42,(uint32_t)file));
 /* A sibling directory exists before the file rename, matching the shell flow. */
 CHECK(!sf_nsc_create(&client,"/disk/docs/sub",NODE_DIR,0,0,0,0755));
 /* rename keeps the tree and the volume in step. */
 CHECK(!sf_nsc_rename(&client,42,"/disk/docs/note.txt","/disk/docs/renamed.txt",0,0));
 CHECK(sf_nsc_open(&client,42,"/","/disk/docs/note.txt",OPEN_READ,0,0)==SF_NOT_FOUND);
 file=sf_nsc_open(&client,42,"/","/disk/docs/renamed.txt",OPEN_READ,0,0);CHECK(file>0);
 memset(out,0,sizeof(out));
 CHECK(sf_nsc_read(&client,42,(uint32_t)file,out,5,0,0)==5&&!memcmp(out,"hello",5)&&!sf_nsc_close(&client,42,(uint32_t)file));
 CHECK(!sf_rw_open(&rw,"/docs/renamed.txt",0,SF_RW_READ,&ino)&&disk_has(ino,"hello",5));
 /* Non-empty rmdir is refused without touching the namespace tree. */
 int dir=sf_nsc_open(&client,42,"/","/disk",OPEN_READ|OPEN_WRITE|OPEN_DIRECTORY,0,0);CHECK(dir>0);
 CHECK(sf_nsc_remove(&client,42,(uint32_t)dir,"docs",0,0,1)==SF_BUSY);
 int probe=sf_nsc_open(&client,42,"/","/disk/docs/renamed.txt",OPEN_READ,0,0);CHECK(probe>0);
 CHECK(!sf_nsc_close(&client,42,(uint32_t)probe)&&!sf_nsc_close(&client,42,(uint32_t)dir));
 /* A directory cannot be moved below itself. */
 CHECK(sf_nsc_rename(&client,42,"/disk/docs","/disk/docs/sub/deep",0,0)==SF_INVALID);
 /* Cross-directory directory rename moves the subtree and its files. */
 CHECK(!sf_nsc_create(&client,"/disk/archive",NODE_DIR,0,0,0,0755));
 CHECK(!sf_nsc_rename(&client,42,"/disk/docs","/disk/archive/docs",0,0));
 CHECK(sf_nsc_open(&client,42,"/","/disk/docs",OPEN_READ,0,0)==SF_NOT_FOUND);
 int moved=sf_nsc_open(&client,42,"/","/disk/archive/docs/renamed.txt",OPEN_READ,0,0);CHECK(moved>0);
 CHECK(!sf_nsc_close(&client,42,(uint32_t)moved));
 CHECK(!sf_rw_open(&rw,"/archive/docs/renamed.txt",0,SF_RW_READ,&ino));
 /* Read-only publication rejects mutation in the backend, not only the tree. */
 dir=sf_nsc_open(&client,42,"/","/disk",OPEN_READ|OPEN_DIRECTORY,0,0);CHECK(dir>0);
 CHECK(!sf_nsc_close(&client,42,(uint32_t)dir));
 sf_nsv_detach_rw();
 CHECK(!sf_nsv_attach_rw(&rw,0));
 CHECK(sf_nsc_create(&client,"/disk/denied",NODE_DIR,0,0,0,0755)==SF_READ_ONLY);
 sf_nsv_detach_rw();
 /* A remount sees the committed tree and byte content again. */
 CHECK(!sf_rw_mount(&rw,dev));
 CHECK(!sf_nsv_attach_rw(&rw,1));
 file=sf_nsc_open(&client,42,"/","/disk/archive/docs/renamed.txt",OPEN_READ,0,0);CHECK(file>0);
 memset(out,0,sizeof(out));
 CHECK(sf_nsc_read(&client,42,(uint32_t)file,out,5,0,0)==5&&!memcmp(out,"hello",5));
 CHECK(!sf_nsc_close(&client,42,(uint32_t)file));
 dir=sf_nsc_open(&client,42,"/","/disk/archive/docs",OPEN_READ|OPEN_WRITE|OPEN_DIRECTORY,0,0);CHECK(dir>0);
 CHECK(!sf_nsc_remove(&client,42,(uint32_t)dir,"renamed.txt",0,0,0));
 CHECK(sf_nsc_open(&client,42,"/","/disk/archive/docs/renamed.txt",OPEN_READ,0,0)==SF_NOT_FOUND);
 CHECK(!sf_nsc_close(&client,42,(uint32_t)dir));
 sf_nsv_detach_rw();
 printf("Namespace RW directory tests passed: %u assertions\n",checks);return 0;
}
