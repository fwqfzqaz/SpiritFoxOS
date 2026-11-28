/* SFRWFS1 over a 512-byte sector device via the sf-fs block adapter.
 *
 * The adapter is the only bridge between SFRWFS1's 4096-byte blocks and the
 * sector protocol sf-init exposes to sf-fs, so exercise it on a real byte
 * array: format, mount, partial writes, remount and write-failure recovery. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sfrw.h"
#include "fs_rw.h"

#define SECTORS 8192u
#define BLOCKS (SECTORS/(SF_RW_BLOCK_BYTES/512u))
#define BYTES ((size_t)SECTORS*512u)
static uint8_t media[BYTES];
static int flushes,writes,fail_write,readonly;
static int checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"FAIL line %d\n",__LINE__);exit(1);}}while(0)

static int sector_read(void*context,uint64_t lba,uint8_t*out){
 (void)context;if(lba>=SECTORS)return SF_INVALID;
 memcpy(out,media+(size_t)lba*512u,512u);return 0;
}
static int sector_write(void*context,uint64_t lba,const uint8_t*in){
 (void)context;if(lba>=SECTORS)return SF_INVALID;
 if(readonly)return SF_READ_ONLY;
 if(fail_write)return SF_DEVICE_GONE;
 writes++;memcpy(media+(size_t)lba*512u,in,512u);return 0;
}
static int sector_flush(void*context){(void)context;flushes++;return 0;}
static SfrwDev device(SfRwIo*io){
 io->read=sector_read;io->write=sector_write;io->flush=sector_flush;io->context=0;
 return sf_rw_io_dev(io);
}
static int write_text(Sfrw*vol,uint32_t ino,const char*text){
 return (int)sf_rw_write(vol,ino,0,0,text,strlen(text));
}
static void read_text(Sfrw*vol,uint32_t ino,char*out,size_t cap){
 long n=sf_rw_read(vol,ino,0,0,out,cap-1);CHECK(n>=0);out[n]=0;
}
static int has_entry(Sfrw*vol,uint32_t dir,const char*want){
 char name[SF_RW_NAME_MAX+1];uint32_t ino;uint8_t type;
 for(uint32_t i=0;;i++){
  int r=sf_rw_readdir(vol,dir,0,i,name,&ino,&type);
  if(r==SF_NOT_FOUND)return 0;
  CHECK(!r);if(!strcmp(name,want))return 1;
 }
}
static uint32_t dir_entry_inode(Sfrw*vol,uint32_t dir,const char*want){
 char name[SF_RW_NAME_MAX+1];uint32_t ino;uint8_t type;
 for(uint32_t i=0;;i++){
  int r=sf_rw_readdir(vol,dir,0,i,name,&ino,&type);
  CHECK(!r);if(!strcmp(name,want))return ino;
 }
}
/* Directory operations through the 4096 B to 512 B adapter: creation,
 * nested readdir, rename (same/cross directory, replacement and loops),
 * permission enforcement, non-empty removal and remount persistence. */
static void directories(Sfrw*vol){
 uint32_t work=0,sub=0,file=0;
 CHECK(!sf_rw_mkdir(vol,"/work",0,0755));
 CHECK(!sf_rw_mkdir(vol,"/work/a",0,0755));
 CHECK(!sf_rw_mkdir(vol,"/work/a/b",0,0755));
 CHECK(!sf_rw_mkdir(vol,"/work/c",0,0755));
 CHECK(!sf_rw_open(vol,"/work",0,SF_RW_READ,&work)&&work>0);
 CHECK(!sf_rw_open(vol,"/work/a",0,SF_RW_READ,&sub)&&sub>0);
 CHECK(has_entry(vol,work,"a")&&has_entry(vol,work,"c"));
 CHECK(has_entry(vol,sub,".")&&has_entry(vol,sub,"..")&&has_entry(vol,sub,"b"));
 CHECK(dir_entry_inode(vol,sub,"..")==work);
 CHECK(dir_entry_inode(vol,work,".")==work);
 CHECK(!sf_rw_create(vol,"/work/a/one.txt",0,0644));
 CHECK(!sf_rw_open(vol,"/work/a/one.txt",0,SF_RW_WRITE,&file));
 CHECK(write_text(vol,file,"one")==3);
 CHECK(!sf_rw_create(vol,"/work/c/two.txt",0,0644));
 CHECK(!sf_rw_open(vol,"/work/c/two.txt",0,SF_RW_WRITE,&file));
 CHECK(write_text(vol,file,"two")==3);
 /* Same-directory rename keeps bytes and the old name disappears. */
 CHECK(!sf_rw_rename(vol,"/work/a/one.txt","/work/a/uno.txt",0));
 CHECK(sf_rw_open(vol,"/work/a/one.txt",0,SF_RW_READ,&file)==SF_NOT_FOUND);
 CHECK(!sf_rw_open(vol,"/work/a/uno.txt",0,SF_RW_READ,&file));
 char text[16];read_text(vol,file,text,sizeof(text));CHECK(!strcmp(text,"one"));
 /* Cross-directory rename moves the inode without copying. */
 CHECK(!sf_rw_rename(vol,"/work/a/uno.txt","/work/c/uno.txt",0));
 CHECK(has_entry(vol,work,"c"));
 CHECK(!sf_rw_open(vol,"/work/c/uno.txt",0,SF_RW_READ,&file));
 read_text(vol,file,text,sizeof(text));CHECK(!strcmp(text,"one"));
 /* Renaming over an existing file replaces its contents atomically. */
 CHECK(!sf_rw_create(vol,"/work/a/replace.txt",0,0644));
 CHECK(!sf_rw_open(vol,"/work/a/replace.txt",0,SF_RW_WRITE,&file));
 CHECK(write_text(vol,file,"new")==3);
 CHECK(!sf_rw_create(vol,"/work/c/replace.txt",0,0644));
 CHECK(!sf_rw_open(vol,"/work/c/replace.txt",0,SF_RW_WRITE,&file));
 CHECK(write_text(vol,file,"old-value")==9);
 CHECK(!sf_rw_rename(vol,"/work/a/replace.txt","/work/c/replace.txt",0));
 CHECK(!sf_rw_open(vol,"/work/c/replace.txt",0,SF_RW_READ,&file));
 read_text(vol,file,text,sizeof(text));CHECK(!strcmp(text,"new"));
 /* Moving a directory rewrites its ".." entry and keeps descendants. */
 uint32_t moved=0,c=0;
 CHECK(!sf_rw_open(vol,"/work/c",0,SF_RW_READ,&c)&&c>0);
 CHECK(!sf_rw_rename(vol,"/work/a","/work/c/a",0));
 CHECK(sf_rw_rename(vol,"/work/a","/work/c/missing",0)==SF_NOT_FOUND);
 CHECK(!sf_rw_open(vol,"/work/c/a",0,SF_RW_READ,&moved)&&moved>0);
 CHECK(has_entry(vol,moved,"b"));
 CHECK(!sf_rw_open(vol,"/work/c/a/b",0,SF_RW_READ,&sub)&&sub>0);
 CHECK(dir_entry_inode(vol,sub,"..")==moved);
 /* A directory cannot be moved inside itself, and the root stays fixed. */
 CHECK(sf_rw_rename(vol,"/work/c/a","/work/c/a/b/deep",0)==SF_INVALID);
 CHECK(sf_rw_rename(vol,"/","/work/root",0)==SF_INVALID);
 /* Non-empty directories are refused before any metadata changes. */
 CHECK(sf_rw_rmdir(vol,"/work/c",0)==SF_INVALID);
 CHECK(sf_rw_rmdir(vol,"/work/c/a",0)==SF_INVALID);
 /* Permission checks: a 0700 directory owned by uid 7 rejects uid 8. */
 uint32_t priv=0;
 CHECK(!sf_rw_mkdir(vol,"/work/priv",0,0755));
 CHECK(!sf_rw_open(vol,"/work/priv",0,SF_RW_READ,&priv)&&priv>0);
 CHECK(!sf_rw_chown(vol,priv,0,7,7));
 CHECK(!sf_rw_chmod(vol,priv,0,0700));
 CHECK(sf_rw_mkdir(vol,"/work/priv/denied",8,0755)==SF_DENIED);
 CHECK(!sf_rw_mkdir(vol,"/work/priv/allowed",7,0755));
 CHECK(sf_rw_rmdir(vol,"/work/priv/allowed",8)==SF_DENIED);
 CHECK(!sf_rw_rmdir(vol,"/work/priv/allowed",7));
 CHECK(!sf_rw_rename(vol,"/work/c/a/b","/work/c/a/kept",0));
 CHECK(has_entry(vol,moved,"kept"));
 CHECK(!sf_rw_rmdir(vol,"/work/c/a/kept",0));
 CHECK(!sf_rw_rmdir(vol,"/work/c/a",0));
 CHECK(!sf_rw_fsync(vol));
 /* Remount through the same adapter and verify the committed tree. */
 memset(vol,0,sizeof(*vol));
 SfRwIo io;SfrwDev dev=device(&io);
 CHECK(!sf_rw_mount(vol,dev));
 CHECK(!sf_rw_open(vol,"/work/c/uno.txt",7,SF_RW_READ,&file));
 read_text(vol,file,text,sizeof(text));CHECK(!strcmp(text,"one"));
 CHECK(sf_rw_open(vol,"/work/c/a",0,SF_RW_READ,&moved)==SF_NOT_FOUND);
 CHECK(has_entry(vol,c,"uno.txt")&&has_entry(vol,c,"replace.txt"));
 CHECK(!sf_rw_open(vol,"/etc/readme.txt",0,SF_RW_READ,&file));
 read_text(vol,file,text,sizeof(text));CHECK(!strcmp(text,"hello"));
 SfckReport report;
 CHECK(!sf_rw_fsck(vol,dev,&report));
 CHECK(!report.lost_inodes&&!report.unreachable_blocks&&!report.bad_entries);
}

int main(void){
 SfRwIo io;Sfrw vol;SfrwDev dev=device(&io);
 SfrwFormatOptions opt;opt.block_count=BLOCKS;opt.default_quota_blocks=0;opt.default_quota_inodes=0;
 memset(media,0,sizeof(media));
 CHECK(!sf_rw_format(&vol,dev,&opt));
 CHECK(!memcmp(media,"SFRWFS1\0",8));
 CHECK(flushes>0);
 /* The adapter maps each 4096-byte block to eight whole sectors. */
 uint8_t block[SF_RW_BLOCK_BYTES];
 CHECK(!sf_rw_io_read(&io,0,block));
 CHECK(!memcmp(block,"SFRWFS1\0",8));
 block[100]^=0x5a;
 CHECK(!sf_rw_io_write(&io,200,block));
 memset(block,0,sizeof(block));
 CHECK(!sf_rw_io_read(&io,200,block));
 CHECK(block[100]==0x5a);
 int base_writes=writes;
 CHECK(!sf_rw_mount(&vol,dev));
 CHECK(!sf_rw_mkdir(&vol,"/etc",0,0755));
 CHECK(!sf_rw_create(&vol,"/etc/readme.txt",0,0644));
 uint32_t ino=0;
 CHECK(!sf_rw_open(&vol,"/etc/readme.txt",0,SF_RW_WRITE,&ino));
 CHECK(ino>0);
 CHECK(write_text(&vol,ino,"hello writable world")==20);
 CHECK(!sf_rw_fsync(&vol));
 SfrwStat st;CHECK(!sf_rw_stat(&vol,ino,&st));CHECK(st.size==20);
 CHECK(st.links==1);
 char text[64];read_text(&vol,ino,text,sizeof(text));CHECK(!strcmp(text,"hello writable world"));
 CHECK(writes>base_writes);
 CHECK(!sf_rw_truncate(&vol,ino,0,5));
 memset(&vol,0,sizeof(vol));
 CHECK(!sf_rw_mount(&vol,dev));
 CHECK(!sf_rw_open(&vol,"/etc/readme.txt",0,SF_RW_READ,&ino));
 read_text(&vol,ino,text,sizeof(text));CHECK(!strcmp(text,"hello"));
 CHECK(flushes>0);
 /* A read-only device rejects writes and forces the volume closed. */
 readonly=1;
 CHECK(write_text(&vol,ino,"x")<0);
 CHECK(!vol.mounted);
 readonly=0;
 memset(&vol,0,sizeof(vol));
 CHECK(!sf_rw_mount(&vol,dev));
 CHECK(!sf_rw_open(&vol,"/etc/readme.txt",0,SF_RW_READ,&ino));
 read_text(&vol,ino,text,sizeof(text));CHECK(!strcmp(text,"hello"));
 /* A failing device surfaces the error and leaves the committed state. */
 fail_write=1;
 CHECK(write_text(&vol,ino,"x")<0);
 CHECK(!vol.mounted);
 fail_write=0;
 memset(&vol,0,sizeof(vol));
 CHECK(!sf_rw_mount(&vol,dev));
 CHECK(!sf_rw_open(&vol,"/etc/readme.txt",0,SF_RW_READ,&ino));
 read_text(&vol,ino,text,sizeof(text));CHECK(!strcmp(text,"hello"));
 directories(&vol);
 printf("FSRW PASS: %d checks, %d sector writes, %d flushes\n",checks,writes,flushes);
 return 0;
}
