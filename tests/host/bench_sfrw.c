/* SFRWFS1 宿主吞吐基准：在内存设备上测量格式、顺序写/读、元数据操作
 * 与 fsck 的 CPU 时间。属于 E1 证据（格式核心自身开销），不含真实块设备
 * 与 QEMU 的中断开销。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sfrw.h"

#define BLOCKS 8192u                          /* 32 MiB 卷，覆盖间接块路径 */
#define CHUNK SF_RW_BLOCK_BYTES               /* 每次写/读一个 4 KiB 块 */
#define DATA_BLOCKS 576u                      /* 2.25 MiB，最后一次 fsync 提交余下窗口 */
#define META_FILES 256u
#define DIR_COUNT 128u                        /* 目录基准规模：保留 inode 余量 */
#define DIR_FILES 4u                          /* 每个目录的文件数，用于 readdir 与清理 */

static uint8_t*disk;
static unsigned long device_writes,device_reads,device_flushes;
static Sfrw vol,fsck_vol;

static int dev_read(void*ctx,uint32_t lba,uint8_t*out){
 (void)ctx;if(lba>=BLOCKS)return SF_INVALID;
 memcpy(out,disk+(size_t)lba*SF_RW_BLOCK_BYTES,SF_RW_BLOCK_BYTES);device_reads++;return 0;
}
static int dev_write(void*ctx,uint32_t lba,const uint8_t*in){
 (void)ctx;if(lba>=BLOCKS)return SF_INVALID;
 memcpy(disk+(size_t)lba*SF_RW_BLOCK_BYTES,in,SF_RW_BLOCK_BYTES);device_writes++;return 0;
}
static int dev_flush(void*ctx){(void)ctx;device_flushes++;return 0;}
static SfrwDev device(void){
 SfrwDev d;d.read=dev_read;d.write=dev_write;d.flush=dev_flush;d.context=0;return d;
}
static double now(void){return (double)clock()/CLOCKS_PER_SEC;}
static void fail(const char*what,int code){
 fprintf(stderr,"BENCH SFRW FAIL: %s (code=%d)\n",what,code);exit(1);
}

int main(void){
 disk=calloc(BLOCKS,SF_RW_BLOCK_BYTES);
 if(!disk)fail("calloc",SF_NO_SPACE);
 uint8_t*buffer=malloc(CHUNK);
 if(!buffer)fail("malloc",SF_NO_SPACE);

 double start=now();
 SfrwFormatOptions opt={BLOCKS,0,0};
 int r=sf_rw_format(&vol,device(),&opt);
 if(r)fail("format",r);
 double format_s=now()-start;

 start=now();
 r=sf_rw_mount(&vol,device());
 if(r)fail("mount",r);
 double mount_s=now()-start;

 uint32_t ino;
 r=sf_rw_create(&vol,"/bench.bin",0,0644);
 if(r)fail("create",r);
 r=sf_rw_open(&vol,"/bench.bin",0,SF_RW_WRITE,&ino);
 if(r)fail("open",r);
 start=now();
 for(unsigned i=0;i<DATA_BLOCKS;i++){
  memset(buffer,(int)(i&0xff),CHUNK);
  long n=sf_rw_write(&vol,ino,0,(uint64_t)i*CHUNK,buffer,CHUNK);
  if(n!=(long)CHUNK)fail("write",(int)n);
 }
 double write_s=now()-start;
 start=now();
 r=sf_rw_fsync(&vol);
 if(r)fail("write fsync",r);
 double write_sync_s=now()-start;

 r=sf_rw_open(&vol,"/bench.bin",0,SF_RW_READ,&ino);
 if(r)fail("reopen",r);
 start=now();
 for(unsigned i=0;i<DATA_BLOCKS;i++){
  long n=sf_rw_read(&vol,ino,0,(uint64_t)i*CHUNK,buffer,CHUNK);
  if(n!=(long)CHUNK)fail("read",(int)n);
  if(buffer[0]!=(uint8_t)(i&0xff))fail("content",i);
 }
 double read_s=now()-start;

 char path[32],other[32];
 start=now();
 for(unsigned i=0;i<META_FILES;i++){
  snprintf(path,sizeof(path),"/f%03u",i);
  r=sf_rw_create(&vol,path,0,0644);
  if(r)fail("meta create",r);
 }
 double create_s=now()-start;
 start=now();
 for(unsigned i=0;i<META_FILES;i++){
  SfrwStat st;uint32_t f;
  snprintf(path,sizeof(path),"/f%03u",i);
  r=sf_rw_open(&vol,path,0,SF_RW_READ,&f);
  if(r)fail("meta open",r);
  r=sf_rw_stat(&vol,f,&st);
  if(r)fail("meta stat",r);
 }
 double stat_s=now()-start;
 start=now();
 for(unsigned i=0;i<META_FILES;i++){
  snprintf(path,sizeof(path),"/f%03u",i);
  snprintf(other,sizeof(other),"/g%03u",i);
  r=sf_rw_rename(&vol,path,other,0);
  if(r)fail("meta rename",r);
 }
 double rename_s=now()-start;
 start=now();
 for(unsigned i=0;i<META_FILES;i++){
  snprintf(path,sizeof(path),"/g%03u",i);
  r=sf_rw_unlink(&vol,path,0);
  if(r)fail("meta unlink",r);
 }
 double unlink_s=now()-start;

 /* Directory operations: mkdir, nested readdir, directory rename and rmdir.
  * Each directory carries DIR_FILES files so that readdir walks real entry
  * blocks and rmdir has to refuse non-empty directories before cleanup. */
 start=now();
 for(unsigned i=0;i<DIR_COUNT;i++){
  snprintf(path,sizeof(path),"/d%03u",i);
  r=sf_rw_mkdir(&vol,path,0,0755);
  if(r)fail("dir mkdir",r);
 }
 double mkdir_s=now()-start;
 for(unsigned i=0;i<DIR_COUNT;i++){
  snprintf(path,sizeof(path),"/d%03u",i);
  for(unsigned f=0;f<DIR_FILES;f++){
   snprintf(other,sizeof(other),"%s/f%u",path,f);
   r=sf_rw_create(&vol,other,0,0644);
   if(r)fail("dir create",r);
  }
 }
 start=now();
 unsigned long entries=0;uint32_t dir=0;
 r=sf_rw_open(&vol,"/",0,SF_RW_READ,&dir);
 if(r)fail("dir root open",r);
 for(unsigned i=0;;i++){
  char name[SF_RW_NAME_MAX+1];uint32_t child=0;uint8_t kind=0;
  r=sf_rw_readdir(&vol,dir,0,i,name,&child,&kind);
  if(r==SF_NOT_FOUND)break;
  if(r)fail("dir readdir root",r);
  entries++;
 }
 for(unsigned i=0;i<DIR_COUNT;i++){
  snprintf(path,sizeof(path),"/d%03u",i);
  r=sf_rw_open(&vol,path,0,SF_RW_READ,&dir);
  if(r)fail("dir open",r);
  for(unsigned j=0;;j++){
   char name[SF_RW_NAME_MAX+1];uint32_t child=0;uint8_t kind=0;
   r=sf_rw_readdir(&vol,dir,0,j,name,&child,&kind);
   if(r==SF_NOT_FOUND)break;
   if(r)fail("dir readdir",r);
   entries++;
  }
 }
 double readdir_s=now()-start;
 start=now();
 for(unsigned i=0;i<DIR_COUNT;i++){
  snprintf(path,sizeof(path),"/d%03u",i);
  snprintf(other,sizeof(other),"/e%03u",i);
  r=sf_rw_rename(&vol,path,other,0);
  if(r)fail("dir rename",r);
 }
 double dir_rename_s=now()-start;
 for(unsigned i=0;i<DIR_COUNT;i++){
  snprintf(path,sizeof(path),"/e%03u",i);
  for(unsigned f=0;f<DIR_FILES;f++){
   snprintf(other,sizeof(other),"%s/f%u",path,f);
   r=sf_rw_unlink(&vol,other,0);
   if(r)fail("dir unlink",r);
  }
 }
 start=now();
 for(unsigned i=0;i<DIR_COUNT;i++){
  snprintf(path,sizeof(path),"/e%03u",i);
  r=sf_rw_rmdir(&vol,path,0);
  if(r)fail("dir rmdir",r);
 }
 double rmdir_s=now()-start;

 unsigned long writes_before=device_writes,flushes_before=device_flushes;
 r=sf_rw_fsync(&vol);
 if(r)fail("fsync",r);
 vol.mounted=0;
 SfckReport report;
 start=now();
 r=sf_rw_fsck(&fsck_vol,device(),&report);
 if(r)fail("fsck",r);
 double fsck_s=now()-start;

 start=now();
 r=sf_rw_mount(&vol,device());
 if(r)fail("remount",r);
 double remount_s=now()-start;

 double mib=(double)DATA_BLOCKS*CHUNK/(1024.0*1024.0);
 printf("RESULT sfrw_volume_mib=%.0f\n",(double)BLOCKS*SF_RW_BLOCK_BYTES/(1024.0*1024.0));
 printf("RESULT sfrw_format_ms=%.1f\n",format_s*1e3);
 printf("RESULT sfrw_mount_ms=%.1f\n",mount_s*1e3);
 printf("RESULT sfrw_write_mib_per_s=%.1f\n",mib/write_s);
 printf("RESULT sfrw_write_us_per_block=%.1f\n",write_s*1e6/DATA_BLOCKS);
 printf("RESULT sfrw_write_fsync_ms=%.1f\n",write_sync_s*1e3);
 printf("RESULT sfrw_read_mib_per_s=%.1f\n",mib/read_s);
 printf("RESULT sfrw_create_per_s=%.0f\n",(double)META_FILES/create_s);
 printf("RESULT sfrw_stat_per_s=%.0f\n",(double)META_FILES/stat_s);
 printf("RESULT sfrw_rename_per_s=%.0f\n",(double)META_FILES/rename_s);
 printf("RESULT sfrw_unlink_per_s=%.0f\n",(double)META_FILES/unlink_s);
 printf("RESULT sfrw_mkdir_per_s=%.0f\n",(double)DIR_COUNT/mkdir_s);
 printf("RESULT sfrw_readdir_entries_per_s=%.0f\n",(double)entries/readdir_s);
 printf("RESULT sfrw_readdir_entries=%lu\n",entries);
 printf("RESULT sfrw_dir_rename_per_s=%.0f\n",(double)DIR_COUNT/dir_rename_s);
 printf("RESULT sfrw_rmdir_per_s=%.0f\n",(double)DIR_COUNT/rmdir_s);
 printf("RESULT sfrw_fsck_ms=%.1f\n",fsck_s*1e3);
 printf("RESULT sfrw_remount_ms=%.1f\n",remount_s*1e3);
 printf("RESULT sfrw_fsync_blocks=%lu\n",device_writes-writes_before+device_flushes-flushes_before);
 printf("RESULT sfrw_fsck_repairs=%u\n",report.repaired);
 printf("BENCH SFRW PASS: write %.1f MiB/s, read %.1f MiB/s, fsck %.1f ms\n",
        mib/write_s,mib/read_s,fsck_s*1e3);
 free(buffer);free(disk);return 0;
}
