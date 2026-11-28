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
 printf("FSRW PASS: %d checks, %d sector writes, %d flushes\n",checks,writes,flushes);
 return 0;
}
