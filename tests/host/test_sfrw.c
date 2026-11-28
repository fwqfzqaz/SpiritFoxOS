/* SFRWFS1 宿主测试：功能、权限、配额、fsck 修复与逐边界的崩溃一致性。
 *
 * 故障模型与 test_txn.c 相同：successful flush 持久化此前的全部写入；在 flush
 * 之前任意子集（含撕裂块）可能以任意顺序到达稳定介质。fail_at 指定第几次
 * 设备写/刷盘失败，mode 0/1/2 分别表示不落盘、半块撕裂落盘、完整落盘。 */
#include <stdio.h>
#include <stdlib.h>
#include "sfrw.h"

#define BLOCKS 512u
#define BYTES ((size_t)BLOCKS*SF_RW_BLOCK_BYTES)

static uint8_t stable[BYTES],cache[BYTES],baseline[BYTES];
static uint8_t dirty[BLOCKS];
static int operations,fail_at,mode,checks;
static Sfrw vol,rec;
static const char seed[]="seed-data";
static uint8_t payload[5000];
static uint8_t pattern[13*SF_RW_BLOCK_BYTES];
static uint8_t output[13*SF_RW_BLOCK_BYTES];

#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"FAIL line %d op %d mode %d\n",__LINE__,fail_at,mode);exit(1);}}while(0)

static int dev_read(void*ctx,uint32_t lba,uint8_t*out){
 (void)ctx;
 if(lba>=BLOCKS)return SF_INVALID;
 memcpy(out,cache+(size_t)lba*SF_RW_BLOCK_BYTES,SF_RW_BLOCK_BYTES);
 return 0;
}
static void persist_subset(void){
 for(uint32_t i=0;i<BLOCKS;i++)
  if(dirty[i]&&((i*17u+(uint32_t)fail_at)%3u)==0)
   memcpy(stable+(size_t)i*SF_RW_BLOCK_BYTES,cache+(size_t)i*SF_RW_BLOCK_BYTES,SF_RW_BLOCK_BYTES);
}
static int dev_write(void*ctx,uint32_t lba,const uint8_t*in){
 (void)ctx;
 if(lba>=BLOCKS)return SF_INVALID;
 if(++operations==fail_at){
  if(mode){persist_subset();size_t n=mode==1?SF_RW_BLOCK_BYTES/2:SF_RW_BLOCK_BYTES;memcpy(stable+(size_t)lba*SF_RW_BLOCK_BYTES,in,n);}
  return SF_DEVICE_GONE;
 }
 memcpy(cache+(size_t)lba*SF_RW_BLOCK_BYTES,in,SF_RW_BLOCK_BYTES);dirty[lba]=1;return 0;
}
static int dev_flush(void*ctx){
 (void)ctx;
 if(++operations==fail_at){if(mode==1)persist_subset();if(mode==2)memcpy(stable,cache,BYTES);return SF_DEVICE_GONE;}
 memcpy(stable,cache,BYTES);memset(dirty,0,sizeof(dirty));return 0;
}
static SfrwDev device(void){
 SfrwDev d;d.read=dev_read;d.write=dev_write;d.flush=dev_flush;d.context=0;return d;
}
static void reset(void){
 memset(stable,0,BYTES);memset(cache,0,BYTES);memset(dirty,0,sizeof(dirty));
 operations=0;fail_at=0;mode=0;
}
static void boot(Sfrw*v){
 memcpy(cache,stable,BYTES);memset(dirty,0,sizeof(dirty));
 CHECK(!sf_rw_mount(v,device()));
}
static void format_blank(void){
 SfrwFormatOptions opt;
 reset();
 opt.block_count=BLOCKS;opt.default_quota_blocks=0;opt.default_quota_inodes=0;
 CHECK(!sf_rw_format(&vol,device(),&opt));
 CHECK(!sf_rw_mount(&vol,device()));
}
static int read_text(Sfrw*v,const char*path,uint32_t uid,char*out,size_t cap){
 SfrwStat st;
 uint32_t ino;
 int r=sf_rw_open(v,path,uid,SF_RW_READ,&ino);if(r)return r;
 r=sf_rw_stat(v,ino,&st);if(r)return r;
 if(st.size>=cap)return SF_NO_SPACE;
 long n=sf_rw_read(v,ino,uid,0,out,(size_t)st.size);
 if(n<0)return (int)n;
 out[n]=0;return (int)n;
}
static void make_file(Sfrw*v,const char*path,uint32_t uid,const char*text){
 uint32_t ino;size_t n=strlen(text);
 CHECK(!sf_rw_create(v,path,uid,0644));
 CHECK(!sf_rw_open(v,path,uid,SF_RW_WRITE,&ino));
 if(n)CHECK(sf_rw_write(v,ino,uid,0,text,n)==(long)n);
 CHECK(!sf_rw_fsync(v));
}
static void file_text(Sfrw*v,const char*path,const char*want){
 char buf[64];
 int n=read_text(v,path,0,buf,sizeof(buf));
 CHECK(n==(int)strlen(want)&&!memcmp(buf,want,(size_t)n));
}
static uint16_t links_of(Sfrw*v,const char*path){
 SfrwStat st;uint32_t ino;
 CHECK(!sf_rw_open(v,path,0,SF_RW_READ,&ino));
 CHECK(!sf_rw_stat(v,ino,&st));
 return (uint16_t)st.links;
}
static uint32_t rng_state;
static uint32_t rng_next(void){rng_state=rng_state*1664525u+1013904223u;return rng_state>>16;}

/* ---------------- 格式与挂载 ---------------- */
static void test_format_mount(void){
 SfrwFormatOptions opt,bad;SfrwDev none;SfrwStat st;
 char name[SF_RW_NAME_MAX+1];uint32_t ino;uint8_t type;
 reset();
 opt.block_count=BLOCKS;opt.default_quota_blocks=0;opt.default_quota_inodes=0;
 bad=opt;bad.block_count=8;
 CHECK(sf_rw_format(&rec,device(),&bad)==SF_INVALID);
 bad.block_count=SF_RW_MAX_BLOCKS+1;
 CHECK(sf_rw_format(&rec,device(),&bad)==SF_INVALID);
 CHECK(sf_rw_format(&rec,device(),0)==SF_INVALID);
 none.read=0;none.write=0;none.flush=0;none.context=0;
 CHECK(sf_rw_format(&rec,none,&opt)==SF_INVALID);
 CHECK(!sf_rw_format(&vol,device(),&opt));
 memset(&rec,0,sizeof(rec));
 CHECK(sf_rw_stat(&rec,SF_RW_ROOT,&st)==SF_DEVICE_GONE);
 CHECK(!sf_rw_mount(&vol,device()));
 CHECK(!sf_rw_stat(&vol,SF_RW_ROOT,&st));
 CHECK(st.inode==SF_RW_ROOT&&(st.mode&SF_RW_TYPE_MASK)==SF_RW_TYPE_DIR);
 CHECK(st.links==2&&st.size==SF_RW_BLOCK_BYTES&&st.blocks==1);
 CHECK(!sf_rw_readdir(&vol,SF_RW_ROOT,0,0,name,&ino,&type));
 CHECK(!strcmp(name,".")&&ino==SF_RW_ROOT&&type==SF_RW_ENTRY_DIR);
 CHECK(!sf_rw_readdir(&vol,SF_RW_ROOT,0,1,name,&ino,&type));
 CHECK(!strcmp(name,"..")&&ino==SF_RW_ROOT&&type==SF_RW_ENTRY_DIR);
 CHECK(sf_rw_readdir(&vol,SF_RW_ROOT,0,2,name,&ino,&type)==SF_NOT_FOUND);
 /* 主超级块损坏时回退备份块；两块都坏则拒绝。 */
 cache[0]^=1;CHECK(!sf_rw_mount(&rec,device()));
 cache[SF_RW_BLOCK_BYTES]^=1;CHECK(sf_rw_mount(&rec,device())==SF_BAD_FORMAT);
 memcpy(cache,stable,BYTES);memset(dirty,0,sizeof(dirty));
}

/* ---------------- 文件读写 ---------------- */
static void test_files(void){
 SfrwStat st;char buf[64];uint32_t ino,bi,si,ci;long n;uint64_t off;
 format_blank();
 CHECK(!sf_rw_create(&vol,"/f",0,0644));
 CHECK(!sf_rw_open(&vol,"/f",0,SF_RW_WRITE,&ino));
 CHECK(sf_rw_write(&vol,ino,0,0,"hello",5)==5);
 CHECK(!sf_rw_stat(&vol,ino,&st));
 CHECK(st.size==5&&st.blocks==1);
 n=sf_rw_read(&vol,ino,0,0,buf,sizeof(buf));
 CHECK(n==5&&!memcmp(buf,"hello",5));
 CHECK(sf_rw_read(&vol,ino,0,1000,buf,sizeof(buf))==0);
 CHECK(sf_rw_write(&vol,ino,0,1,"EY",2)==2);
 n=sf_rw_read(&vol,ino,0,0,buf,sizeof(buf));
 CHECK(n==5&&!memcmp(buf,"hEYlo",5));
 /* 跨 12 个直接块与间接块。 */
 CHECK(!sf_rw_create(&vol,"/big",0,0644));
 CHECK(!sf_rw_open(&vol,"/big",0,SF_RW_WRITE,&bi));
 CHECK(sf_rw_write(&vol,bi,0,0,pattern,sizeof(pattern))==(long)sizeof(pattern));
 CHECK(!sf_rw_stat(&vol,bi,&st));
 CHECK(st.size==sizeof(pattern)&&st.blocks==14);
 memset(output,0,sizeof(output));
 CHECK(sf_rw_read(&vol,bi,0,0,output,sizeof(output))==(long)sizeof(output));
 CHECK(!memcmp(output,pattern,sizeof(pattern)));
 /* 稀疏洞与跨块读。 */
 CHECK(!sf_rw_create(&vol,"/sparse",0,0644));
 CHECK(!sf_rw_open(&vol,"/sparse",0,SF_RW_WRITE,&si));
 CHECK(sf_rw_write(&vol,si,0,1048576,"Z",1)==1);
 CHECK(!sf_rw_stat(&vol,si,&st));
 CHECK(st.size==1048577&&st.blocks==2);
 CHECK(sf_rw_read(&vol,si,0,1048576,buf,1)==1&&buf[0]=='Z');
 memset(buf,0xAA,sizeof(buf));
 CHECK(sf_rw_read(&vol,si,0,1000000,buf,16)==16);
 for(unsigned i=0;i<16;i++)CHECK(buf[i]==0);
 /* 截断收缩与稀疏扩展。 */
 CHECK(!sf_rw_truncate(&vol,si,0,0));
 CHECK(!sf_rw_stat(&vol,si,&st));CHECK(st.size==0&&st.blocks==0);
 CHECK(!sf_rw_truncate(&vol,si,0,8192));
 CHECK(!sf_rw_stat(&vol,si,&st));CHECK(st.size==8192&&st.blocks==0);
 memset(output,0xAA,8192);
 CHECK(sf_rw_read(&vol,si,0,0,output,8192)==8192);
 for(unsigned i=0;i<8192;i++)CHECK(output[i]==0);
 /* 文件大小上限与卷容量。 */
 CHECK(sf_rw_write(&vol,bi,0,SF_RW_FILE_MAX,buf,0)==0);
 CHECK(sf_rw_write(&vol,bi,0,SF_RW_FILE_MAX,"x",1)==SF_NO_SPACE);
 CHECK(sf_rw_truncate(&vol,bi,0,SF_RW_FILE_MAX+1)==SF_NO_SPACE);
 CHECK(!sf_rw_create(&vol,"/cap",0,0644));
 CHECK(!sf_rw_open(&vol,"/cap",0,SF_RW_WRITE,&ci));
 off=0;
 while((n=sf_rw_write(&vol,ci,0,off,payload,SF_RW_BLOCK_BYTES))==(long)SF_RW_BLOCK_BYTES)off+=SF_RW_BLOCK_BYTES;
 CHECK(n==SF_NO_SPACE);
 CHECK(off>=100ull*SF_RW_BLOCK_BYTES);
 CHECK(!sf_rw_stat(&vol,ci,&st));CHECK(st.size==off);
}

/* ---------------- 目录与路径 ---------------- */
static void test_dirs(void){
 char name[SF_RW_NAME_MAX+1],longname[200],longpath[400];uint32_t ino;uint8_t type;
 static const char *const invalid[]={"","f","/a/","/a//b","/a/./b","/a/../b","/a\\b","/a b","/a/.."};
 format_blank();
 CHECK(!sf_rw_mkdir(&vol,"/d",0,0755));
 CHECK(sf_rw_mkdir(&vol,"/d",0,0755)==SF_INVALID);
 CHECK(!sf_rw_mkdir(&vol,"/e",0,0755));
 CHECK(!sf_rw_create(&vol,"/d/f",0,0644));
 CHECK(!sf_rw_open(&vol,"/d",0,SF_RW_READ,&ino));
 uint32_t ent=0;
 CHECK(!sf_rw_readdir(&vol,ino,0,0,name,&ent,&type));
 CHECK(!strcmp(name,".")&&ent==ino);
 CHECK(!sf_rw_readdir(&vol,ino,0,1,name,&ent,&type));
 CHECK(!strcmp(name,"..")&&ent==SF_RW_ROOT);
 CHECK(!sf_rw_readdir(&vol,ino,0,2,name,&ent,&type));
 CHECK(!strcmp(name,"f")&&type==SF_RW_ENTRY_FILE);
 CHECK(sf_rw_readdir(&vol,ino,0,3,name,&ent,&type)==SF_NOT_FOUND);
 CHECK(sf_rw_rmdir(&vol,"/d",0)==SF_INVALID);
 CHECK(!sf_rw_unlink(&vol,"/d/f",0));
 CHECK(sf_rw_unlink(&vol,"/d/f",0)==SF_NOT_FOUND);
 CHECK(!sf_rw_rmdir(&vol,"/d",0));
 CHECK(sf_rw_rmdir(&vol,"/d",0)==SF_NOT_FOUND);
 CHECK(sf_rw_rmdir(&vol,"/",0)==SF_INVALID);
 CHECK(sf_rw_unlink(&vol,"/e",0)==SF_INVALID);
 CHECK(!sf_rw_rmdir(&vol,"/e",0));
 CHECK(sf_rw_create(&vol,"/nope/f",0,0644)==SF_NOT_FOUND);
 CHECK(sf_rw_mkdir(&vol,"/nope/d",0,0755)==SF_NOT_FOUND);
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
  CHECK(sf_rw_create(&vol,invalid[i],0,0644)==SF_INVALID);
  CHECK(sf_rw_mkdir(&vol,invalid[i],0,0755)==SF_INVALID);
 }
 longname[0]='/';for(int i=0;i<96;i++)longname[1+i]='a';longname[97]=0;
 CHECK(sf_rw_create(&vol,longname,0,0644)==SF_INVALID);
 {
  size_t p=0;longpath[p++]='/';
  for(int s=0;s<6;s++){for(int i=0;i<50;i++)longpath[p++]='x';if(s<5)longpath[p++]='/';}
  longpath[p]=0;
  CHECK(sf_rw_create(&vol,longpath,0,0644)==SF_INVALID);
 }
 CHECK(!sf_rw_create(&vol,"/dup",0,0644));
 CHECK(sf_rw_create(&vol,"/dup",0,0644)==SF_INVALID);
}

/* ---------------- 权限 ---------------- */
static void test_permissions(void){
 uint32_t ino;
 format_blank();
 CHECK(!sf_rw_mkdir(&vol,"/u",0,0777));
 CHECK(!sf_rw_create(&vol,"/u/open",7,0644));
 CHECK(!sf_rw_open(&vol,"/u/open",8,SF_RW_READ,&ino));
 CHECK(sf_rw_open(&vol,"/u/open",8,SF_RW_WRITE,&ino)==SF_DENIED);
 CHECK(!sf_rw_open(&vol,"/u/open",7,SF_RW_WRITE,&ino));
 CHECK(!sf_rw_create(&vol,"/u/eight",8,0644));
 CHECK(sf_rw_create(&vol,"/rootfile",8,0644)==SF_DENIED);
 CHECK(!sf_rw_mkdir(&vol,"/ro",0,0755));
 CHECK(sf_rw_create(&vol,"/ro/f",8,0644)==SF_DENIED);
 CHECK(!sf_rw_mkdir(&vol,"/nx",0,0600));
 CHECK(!sf_rw_create(&vol,"/nx/f",0,0644));
 CHECK(sf_rw_open(&vol,"/nx/f",8,SF_RW_READ,&ino)==SF_DENIED);
 CHECK(!sf_rw_open(&vol,"/nx/f",0,SF_RW_READ,&ino));
 CHECK(!sf_rw_mkdir(&vol,"/u/own",7,0700));
 CHECK(!sf_rw_create(&vol,"/u/own/f",7,0600));
 CHECK(!sf_rw_open(&vol,"/u/own/f",7,SF_RW_READ,&ino));
 CHECK(sf_rw_open(&vol,"/u/own/f",8,SF_RW_READ,&ino)==SF_DENIED);
 CHECK(sf_rw_open(&vol,"/u/own/f",0,SF_RW_READ,&ino)==0);
}

/* ---------------- 配额 ---------------- */
static void test_quota(void){
 uint32_t ino;
 format_blank();
 CHECK(!sf_rw_mkdir(&vol,"/u",0,0777));
 CHECK(sf_rw_set_quota(&vol,5,5,10,10)==SF_DENIED);
 CHECK(!sf_rw_set_quota(&vol,0,5,0,1));
 CHECK(!sf_rw_create(&vol,"/u/a",5,0644));
 CHECK(sf_rw_create(&vol,"/u/b",5,0644)==SF_NO_SPACE);
 CHECK(!sf_rw_set_quota(&vol,0,5,2,10));
 CHECK(!sf_rw_open(&vol,"/u/a",5,SF_RW_WRITE,&ino));
 CHECK(sf_rw_write(&vol,ino,5,0,payload,SF_RW_BLOCK_BYTES)==(long)SF_RW_BLOCK_BYTES);
 CHECK(sf_rw_write(&vol,ino,5,SF_RW_BLOCK_BYTES,payload,SF_RW_BLOCK_BYTES)==(long)SF_RW_BLOCK_BYTES);
 CHECK(sf_rw_write(&vol,ino,5,2*SF_RW_BLOCK_BYTES,payload,1)==SF_NO_SPACE);
 /* uid 0 不受配额限制。 */
 CHECK(!sf_rw_set_quota(&vol,0,0,1,1));
 CHECK(!sf_rw_create(&vol,"/r1",0,0644));
 CHECK(!sf_rw_create(&vol,"/r2",0,0644));
 CHECK(!sf_rw_open(&vol,"/r1",0,SF_RW_WRITE,&ino));
 CHECK(sf_rw_write(&vol,ino,0,0,payload,SF_RW_BLOCK_BYTES)==(long)SF_RW_BLOCK_BYTES);
}
/* ---------------- 重命名 ---------------- */
static void test_rename(void){
 SfrwStat st;SfrwStat st2;uint32_t ino,tgt;char name[SF_RW_NAME_MAX+1];uint8_t type;
 format_blank();
 make_file(&vol,"/a",0,"AAAA");
 make_file(&vol,"/b",0,"BBBB");
 CHECK(!sf_rw_rename(&vol,"/a","/c",0));
 CHECK(sf_rw_open(&vol,"/a",0,SF_RW_READ,&ino)==SF_NOT_FOUND);
 file_text(&vol,"/c","AAAA");
 CHECK(!sf_rw_rename(&vol,"/c","/b",0));
 CHECK(sf_rw_open(&vol,"/c",0,SF_RW_READ,&ino)==SF_NOT_FOUND);
 file_text(&vol,"/b","AAAA");
 CHECK(!sf_rw_mkdir(&vol,"/d1",0,0755));
 CHECK(!sf_rw_mkdir(&vol,"/d2",0,0755));
 CHECK(!sf_rw_rename(&vol,"/d1","/d2",0));
 CHECK(sf_rw_open(&vol,"/d1",0,SF_RW_READ,&ino)==SF_NOT_FOUND);
 CHECK(!sf_rw_open(&vol,"/d2",0,SF_RW_READ,&ino));
 CHECK(!sf_rw_stat(&vol,ino,&st));CHECK((st.mode&SF_RW_TYPE_MASK)==SF_RW_TYPE_DIR);
 CHECK(!sf_rw_readdir(&vol,ino,0,1,name,&tgt,&type));
 CHECK(!strcmp(name,"..")&&tgt==SF_RW_ROOT);
 CHECK(!sf_rw_mkdir(&vol,"/d3",0,0755));
 make_file(&vol,"/d3/x",0,"X");
 CHECK(sf_rw_rename(&vol,"/d2","/d3",0)==SF_INVALID);
 CHECK(sf_rw_rename(&vol,"/b","/d3",0)==SF_INVALID);
 CHECK(sf_rw_rename(&vol,"/d3","/b",0)==SF_INVALID);
 CHECK(!sf_rw_rename(&vol,"/b","/b",0));
 CHECK(!sf_rw_mkdir(&vol,"/m",0,0755));
 CHECK(!sf_rw_rename(&vol,"/b","/m/b",0));
 file_text(&vol,"/m/b","AAAA");
 CHECK(!sf_rw_mkdir(&vol,"/m/sub",0,0755));
 CHECK(links_of(&vol,"/m")==3);
 CHECK(!sf_rw_mkdir(&vol,"/n",0,0755));
 CHECK(!sf_rw_rename(&vol,"/m/sub","/n/sub",0));
 CHECK(links_of(&vol,"/m")==2);
 CHECK(links_of(&vol,"/n")==3);
 CHECK(!sf_rw_open(&vol,"/n/sub",0,SF_RW_READ,&ino));
 CHECK(!sf_rw_readdir(&vol,ino,0,1,name,&tgt,&type));
 CHECK(!strcmp(name,".."));
 CHECK(!sf_rw_open(&vol,"/n",0,SF_RW_READ,&tgt));
 CHECK(!sf_rw_stat(&vol,tgt,&st2));CHECK(st2.links==3);
 CHECK(sf_rw_rename(&vol,"/n","/n/sub/x",0)==SF_INVALID);
 CHECK(sf_rw_rename(&vol,"/nope","/x",0)==SF_NOT_FOUND);
 CHECK(sf_rw_rename(&vol,"/m/b","/missing/x",0)==SF_NOT_FOUND);
}

/* ---------------- 硬链接 ---------------- */
static void test_link(void){
 uint32_t ino;SfrwStat st;
 format_blank();
 make_file(&vol,"/a",0,"LINK");
 CHECK(links_of(&vol,"/a")==1);
 CHECK(!sf_rw_link(&vol,"/a","/b",0));
 CHECK(links_of(&vol,"/a")==2&&links_of(&vol,"/b")==2);
 file_text(&vol,"/b","LINK");
 /* 删除一个名字只递减计数，数据仍可经另一名字访问。 */
 CHECK(!sf_rw_unlink(&vol,"/a",0));
 CHECK(links_of(&vol,"/b")==1);
 file_text(&vol,"/b","LINK");
 CHECK(sf_rw_open(&vol,"/a",0,SF_RW_READ,&ino)==SF_NOT_FOUND);
 /* 跨目录链接与目标已存在拒绝。 */
 CHECK(!sf_rw_mkdir(&vol,"/d",0,0755));
 CHECK(!sf_rw_link(&vol,"/b","/d/b2",0));
 CHECK(links_of(&vol,"/b")==2);
 file_text(&vol,"/d/b2","LINK");
 CHECK(sf_rw_link(&vol,"/b","/d/b2",0)==SF_INVALID);
 /* 目录不能作为链接源。 */
 CHECK(sf_rw_link(&vol,"/d","/d2",0)==SF_INVALID);
 CHECK(sf_rw_link(&vol,"/missing","/x",0)==SF_NOT_FOUND);
 /* 非属主在无写权限目录中创建链接被拒绝。 */
 CHECK(sf_rw_link(&vol,"/b","/d/denied",8)==SF_DENIED);
 /* 链接计数持久化。 */
 boot(&vol);
 CHECK(links_of(&vol,"/b")==2&&links_of(&vol,"/d/b2")==2);
 CHECK(!sf_rw_open(&vol,"/b",0,SF_RW_READ,&ino));
 CHECK(!sf_rw_stat(&vol,ino,&st));
 CHECK(st.blocks==1&&st.size==4);
 CHECK(!sf_rw_unlink(&vol,"/b",0));
 CHECK(!sf_rw_unlink(&vol,"/d/b2",0));
 CHECK(sf_rw_open(&vol,"/b",0,SF_RW_READ,&ino)==SF_NOT_FOUND);
}

/* ---------------- chmod / chown ---------------- */
static void test_chmod(void){
 uint32_t ino,g;SfrwStat st;
 format_blank();
 make_file(&vol,"/f",0,"X");
 CHECK(!sf_rw_open(&vol,"/f",0,SF_RW_READ,&ino));
 CHECK(!sf_rw_chmod(&vol,ino,0,0600));
 CHECK(!sf_rw_stat(&vol,ino,&st));
 CHECK((st.mode&SF_RW_PERM_MASK)==0600);
 CHECK((st.mode&SF_RW_TYPE_MASK)==SF_RW_TYPE_FILE);
 /* 拒绝类型位/超出权限位。 */
 CHECK(sf_rw_chmod(&vol,ino,0,(uint16_t)(SF_RW_TYPE_DIR|0644))==SF_INVALID);
 /* 非属主不能修改；uid 0 与属主可以。 */
 CHECK(sf_rw_chmod(&vol,ino,8,0644)==SF_DENIED);
 CHECK(!sf_rw_chmod(&vol,ino,0,0644));
 CHECK(!sf_rw_mkdir(&vol,"/u",0,0777));
 CHECK(!sf_rw_create(&vol,"/u/g",7,0644));
 CHECK(!sf_rw_open(&vol,"/u/g",7,SF_RW_READ,&g));
 CHECK(!sf_rw_chmod(&vol,g,7,0600));
 CHECK(!sf_rw_stat(&vol,g,&st));CHECK((st.mode&SF_RW_PERM_MASK)==0600);
 /* 收紧后其他 uid 读取被拒绝。 */
 CHECK(sf_rw_open(&vol,"/u/g",8,SF_RW_READ,&g)==SF_DENIED);
}
static void test_chown(void){
 uint32_t ino,b;SfrwStat st;
 format_blank();
 make_file(&vol,"/f",0,"OWN");
 CHECK(!sf_rw_open(&vol,"/f",0,SF_RW_READ,&ino));
 /* 仅 uid 0 可 chown。 */
 CHECK(sf_rw_chown(&vol,ino,7,7,7)==SF_DENIED);
 CHECK(!sf_rw_chown(&vol,ino,0,7,7));
 CHECK(!sf_rw_stat(&vol,ino,&st));CHECK(st.uid==7&&st.gid==7);
 CHECK(!sf_rw_open(&vol,"/f",8,SF_RW_READ,&ino));
 CHECK(sf_rw_open(&vol,"/f",8,SF_RW_WRITE,&ino)==SF_DENIED);
 /* 目标 uid 配额不足时失败且原属主不变。 */
 CHECK(!sf_rw_mkdir(&vol,"/q",0,0777));
 CHECK(!sf_rw_set_quota(&vol,0,9,100,1));
 CHECK(!sf_rw_create(&vol,"/q/a",9,0644));
 CHECK(!sf_rw_create(&vol,"/q/b",0,0644));
 CHECK(!sf_rw_open(&vol,"/q/b",0,SF_RW_READ,&b));
 CHECK(sf_rw_chown(&vol,b,0,9,9)==SF_NO_SPACE);
 CHECK(!sf_rw_stat(&vol,b,&st));CHECK(st.uid==0);
 /* 不存在的 inode 与非 root 调用。 */
 CHECK(sf_rw_chown(&vol,600,0,1,1)==SF_NOT_FOUND);
}

/* ---------------- fsck ---------------- */
static uint8_t* raw_inode(uint32_t ino){
 return cache+(size_t)(SF_RW_INODE_TABLE_BLOCK+ino/SF_RW_INODES_PER_BLOCK)*SF_RW_BLOCK_BYTES
  +(size_t)(ino%SF_RW_INODES_PER_BLOCK)*SF_RW_INODE_BYTES;
}
static uint32_t raw_u32(const uint8_t*p){uint32_t v;memcpy(&v,p,4);return v;}
static void raw_set32(uint8_t*p,uint32_t v){memcpy(p,&v,4);}
static void raw_set16(uint8_t*p,uint16_t v){memcpy(p,&v,2);}

static void fsck_clean(void){
 SfckReport r;SfrwStat st;
 format_blank();
 make_file(&vol,"/keep",0,"KEEP");
 CHECK(!sf_rw_mkdir(&vol,"/sub",0,0755));
 make_file(&vol,"/sub/inner",0,"INNER");
 memset(&r,0,sizeof(r));
 CHECK(!sf_rw_fsck(&rec,device(),&r));
 CHECK(r.repaired==0&&r.lost_inodes==0&&r.bad_entries==0);
 CHECK(r.duplicate_blocks==0&&r.fixed_links==0&&r.unreachable_blocks==0);
 CHECK(!sf_rw_mount(&rec,device()));
 file_text(&rec,"/keep","KEEP");
 file_text(&rec,"/sub/inner","INNER");
 CHECK(!sf_rw_stat(&rec,SF_RW_ROOT,&st));CHECK(st.links==3);
}
static void fsck_bad_pointer(void){
 SfckReport r;SfrwStat st;uint32_t ino;char buf[8];
 format_blank();
 CHECK(!sf_rw_create(&vol,"/bad",0,0644));
 CHECK(!sf_rw_open(&vol,"/bad",0,SF_RW_WRITE,&ino));
 CHECK(sf_rw_write(&vol,ino,0,0,"BAD",3)==3);
 CHECK(!sf_rw_mkdir(&vol,"/d",0,0755));
 raw_set32(raw_inode(ino)+24,2);
 CHECK(sf_rw_mount(&rec,device())==SF_BAD_FORMAT);
 memset(&r,0,sizeof(r));
 CHECK(!sf_rw_fsck(&rec,device(),&r));
 CHECK(r.repaired>=1);
 CHECK(!sf_rw_mount(&rec,device()));
 CHECK(read_text(&rec,"/bad",0,buf,sizeof(buf))==3);
 for(unsigned i=0;i<3;i++)CHECK(buf[i]==0);
 CHECK(!sf_rw_stat(&rec,ino,&st));CHECK(st.size==3&&st.blocks==0);
}
static void fsck_duplicate(void){
 SfckReport r;uint32_t a,b,ab,bb;char buf[8];
 format_blank();
 make_file(&vol,"/aa",0,"AAAA");
 make_file(&vol,"/bb",0,"BBBB");
 CHECK(!sf_rw_open(&vol,"/aa",0,SF_RW_WRITE,&a));
 CHECK(!sf_rw_open(&vol,"/bb",0,SF_RW_WRITE,&b));
 ab=raw_u32(raw_inode(a)+24);bb=raw_u32(raw_inode(b)+24);
 CHECK(ab>=SF_RW_DATA_BLOCK&&bb>=SF_RW_DATA_BLOCK&&ab!=bb);
 raw_set32(raw_inode(b)+24,ab);
 CHECK(!sf_rw_mount(&rec,device()));
 memset(&r,0,sizeof(r));
 CHECK(!sf_rw_fsck(&rec,device(),&r));
 CHECK(r.duplicate_blocks>=1&&r.unreachable_blocks>=1);
 CHECK(!sf_rw_mount(&rec,device()));
 file_text(&rec,"/aa","AAAA");
 CHECK(read_text(&rec,"/bb",0,buf,sizeof(buf))==4);
 for(unsigned i=0;i<4;i++)CHECK(buf[i]==0);
}
static void fsck_lost_inode(void){
 SfckReport r;SfrwStat st;uint32_t fake=500;uint8_t*ip;
 format_blank();
 ip=raw_inode(fake);memset(ip,0,SF_RW_INODE_BYTES);
 raw_set16(ip,(uint16_t)(SF_RW_TYPE_FILE|0644));raw_set16(ip+2,1);
 CHECK(sf_rw_mount(&rec,device())==SF_BAD_FORMAT);
 memset(&r,0,sizeof(r));
 CHECK(!sf_rw_fsck(&rec,device(),&r));
 CHECK(r.lost_inodes==1&&r.repaired>=1);
 CHECK(!sf_rw_mount(&rec,device()));
 CHECK(sf_rw_stat(&rec,fake,&st)==SF_NOT_FOUND);
}
static void fsck_bad_entry(void){
 SfckReport r;char buf[8];uint8_t*root;int found=0;
 format_blank();
 make_file(&vol,"/keep",0,"KEEP");
 make_file(&vol,"/gone",0,"GONE");
 root=cache+(size_t)SF_RW_DATA_BLOCK*SF_RW_BLOCK_BYTES;
 for(unsigned i=0;i<SF_RW_BLOCK_BYTES/128;i++){
  uint8_t*e=root+i*128;
  if(raw_u32(e)&&e[4]==4&&!memcmp(e+8,"gone",4)){e[4]=120;found=1;break;}
 }
 CHECK(found);
 CHECK(!sf_rw_mount(&rec,device()));
 memset(&r,0,sizeof(r));
 CHECK(!sf_rw_fsck(&rec,device(),&r));
 CHECK(r.bad_entries>=1&&r.lost_inodes==1);
 CHECK(!sf_rw_mount(&rec,device()));
 file_text(&rec,"/keep","KEEP");
 CHECK(read_text(&rec,"/gone",0,buf,sizeof(buf))==SF_NOT_FOUND);
}
static void fsck_dir_metadata(void){
 SfckReport r;uint32_t d,e;uint8_t t;char name[SF_RW_NAME_MAX+1];
 format_blank();
 make_file(&vol,"/keep",0,"KEEP");
 CHECK(!sf_rw_mkdir(&vol,"/d",0,0755));
 CHECK(!sf_rw_open(&vol,"/d",0,SF_RW_READ,&d));
 CHECK(raw_u32(raw_inode(d)+24)>=SF_RW_DATA_BLOCK);
 /* 目录块指针指向 inode 表块：严格挂载失败，fsck 不得读写该元数据块。 */
 raw_set32(raw_inode(d)+24,3);
 CHECK(sf_rw_mount(&rec,device())==SF_BAD_FORMAT);
 memset(&r,0,sizeof(r));
 CHECK(!sf_rw_fsck(&rec,device(),&r));
 CHECK(r.bad_entries>=1&&r.repaired>=1);
 for(size_t i=0;i<SF_RW_BLOCK_BYTES;i++)CHECK(cache[(size_t)3*SF_RW_BLOCK_BYTES+i]==0);
 CHECK(!sf_rw_mount(&rec,device()));
 file_text(&rec,"/keep","KEEP");
 CHECK(!sf_rw_open(&rec,"/d",0,SF_RW_READ,&d));
 CHECK(!sf_rw_readdir(&rec,d,0,0,name,&e,&t));
 CHECK(!strcmp(name,".")&&e==d);
}
static void fsck_beyond_size(void){
 SfckReport r;SfrwStat st;uint32_t f,g;char buf[8];
 format_blank();
 make_file(&vol,"/f",0,"ABC");
 CHECK(!sf_rw_open(&vol,"/f",0,SF_RW_READ,&f));
 CHECK(!sf_rw_create(&vol,"/g",0,0644));
 CHECK(!sf_rw_open(&vol,"/g",0,SF_RW_WRITE,&g));
 CHECK(sf_rw_write(&vol,g,0,0,"GGGG",4)==4);
 CHECK(!sf_rw_fsync(&vol));
 uint32_t gb=raw_u32(raw_inode(g)+24);
 CHECK(gb>=SF_RW_DATA_BLOCK);
 /* /f 的第二个直接块超出 size，且与 /g 重复；fsck 应先清除而不计重复占用。 */
 raw_set32(raw_inode(f)+24+4,gb);
 CHECK(sf_rw_mount(&rec,device())==SF_BAD_FORMAT);
 memset(&r,0,sizeof(r));
 CHECK(!sf_rw_fsck(&rec,device(),&r));
 CHECK(r.repaired>=1&&r.duplicate_blocks==0);
 CHECK(!sf_rw_mount(&rec,device()));
 CHECK(!sf_rw_stat(&rec,f,&st));CHECK(st.size==3&&st.blocks==1);
 CHECK(read_text(&rec,"/f",0,buf,sizeof(buf))==3&&!memcmp(buf,"ABC",3));
 file_text(&rec,"/g","GGGG");
}
static void test_fsck(void){
 fsck_clean();
 fsck_bad_pointer();
 fsck_duplicate();
 fsck_lost_inode();
 fsck_bad_entry();
 fsck_dir_metadata();
 fsck_beyond_size();
}

/* ---------------- 崩溃一致性 ---------------- */
static void recover(void){
 memcpy(cache,stable,BYTES);memset(dirty,0,sizeof(dirty));
 operations=0;fail_at=0;mode=0;
 if(sf_rw_mount(&rec,device())){
  SfckReport rep;memset(&rep,0,sizeof(rep));
  CHECK(!sf_rw_fsck(&rec,device(),&rep));
  memcpy(cache,stable,BYTES);memset(dirty,0,sizeof(dirty));
  CHECK(!sf_rw_mount(&rec,device()));
 }
}
static void check_crash_state(Sfrw*v){
 char buf[32];uint32_t ino;SfrwStat st;
 int n=read_text(v,"/seed",0,buf,sizeof(buf));
 CHECK(n==(int)strlen(seed)&&!memcmp(buf,seed,strlen(seed)));
 int r=sf_rw_open(v,"/crash",0,SF_RW_READ,&ino);
 if(r==SF_NOT_FOUND)return;
 CHECK(!r);
 CHECK(!sf_rw_stat(v,ino,&st));
 CHECK(st.size==0||st.size==sizeof(payload));
 if(st.size==sizeof(payload)){
  CHECK(sf_rw_read(v,ino,0,0,output,sizeof(payload))==(long)sizeof(payload));
  CHECK(!memcmp(output,payload,sizeof(payload)));
 }
}
static void crash_create_write(void){
 uint32_t ino;int total;
 format_blank();
 make_file(&vol,"/seed",0,seed);
 memcpy(baseline,stable,BYTES);
 boot(&vol);
 operations=0;fail_at=0;mode=0;
 CHECK(!sf_rw_create(&vol,"/crash",0,0644));
 CHECK(!sf_rw_open(&vol,"/crash",0,SF_RW_WRITE,&ino));
 CHECK(sf_rw_write(&vol,ino,0,0,payload,sizeof(payload))==(long)sizeof(payload));
 total=operations;
 CHECK(total>0);
 check_crash_state(&vol);
 for(int m=0;m<3;m++)for(int f=1;f<=total;f++){
  long w=0;int r;
  memcpy(stable,baseline,BYTES);
  boot(&vol);
  operations=0;fail_at=f;mode=m;
  r=sf_rw_create(&vol,"/crash",0,0644);
  if(!r){
   uint32_t ci;
   if(!sf_rw_open(&vol,"/crash",0,SF_RW_WRITE,&ci))w=sf_rw_write(&vol,ci,0,0,payload,sizeof(payload));
   else w=SF_DEVICE_GONE;
  }
  CHECK(r!=0||w!=(long)sizeof(payload));
  fail_at=0;mode=0;operations=0;
  recover();
  check_crash_state(&rec);
 }
 fail_at=0;mode=0;
}
static void crash_rename(void){
 static const char A[]="from-data";
 static const char B[]="to-data";
 int total;uint32_t ino;
 format_blank();
 make_file(&vol,"/from",0,A);
 make_file(&vol,"/to",0,B);
 memcpy(baseline,stable,BYTES);
 boot(&vol);
 operations=0;fail_at=0;mode=0;
 CHECK(!sf_rw_rename(&vol,"/from","/to",0));
 total=operations;
 CHECK(total>0);
 file_text(&vol,"/to",A);
 CHECK(sf_rw_open(&vol,"/from",0,SF_RW_READ,&ino)==SF_NOT_FOUND);
 for(int m=0;m<3;m++)for(int f=1;f<=total;f++){
  char fb[32],tb[32];int fn,tn,oldstate,newstate;
  memcpy(stable,baseline,BYTES);
  boot(&vol);
  operations=0;fail_at=f;mode=m;
  CHECK(sf_rw_rename(&vol,"/from","/to",0)!=0);
  fail_at=0;mode=0;operations=0;
  recover();
  fn=read_text(&rec,"/from",0,fb,sizeof(fb));
  tn=read_text(&rec,"/to",0,tb,sizeof(tb));
  oldstate=fn==(int)strlen(A)&&!memcmp(fb,A,strlen(A))&&
           tn==(int)strlen(B)&&!memcmp(tb,B,strlen(B));
  newstate=fn==SF_NOT_FOUND&&tn==(int)strlen(A)&&!memcmp(tb,A,strlen(A));
  CHECK(oldstate||newstate);
 }
 fail_at=0;mode=0;
}

/* ---------------- 随机掉电注入 ---------------- */
static void crash_random(void){
 static const char A[]="from-data";
 static const char B[]="to-data";
 int total;uint32_t ino;
 /* 场景 1：create+write，固定种子伪随机选取写/刷盘边界与故障模式。 */
 format_blank();
 make_file(&vol,"/seed",0,seed);
 memcpy(baseline,stable,BYTES);
 boot(&vol);
 operations=0;fail_at=0;mode=0;
 CHECK(!sf_rw_create(&vol,"/crash",0,0644));
 CHECK(!sf_rw_open(&vol,"/crash",0,SF_RW_WRITE,&ino));
 CHECK(sf_rw_write(&vol,ino,0,0,payload,sizeof(payload))==(long)sizeof(payload));
 total=operations;CHECK(total>0);
 rng_state=0x20261128u;
 for(int i=0;i<256;i++){
  long w=0;int r;
  memcpy(stable,baseline,BYTES);
  boot(&vol);
  operations=0;fail_at=1+(int)(rng_next()%(uint32_t)total);mode=(int)(rng_next()%3u);
  r=sf_rw_create(&vol,"/crash",0,0644);
  if(!r){
   uint32_t ci;
   if(!sf_rw_open(&vol,"/crash",0,SF_RW_WRITE,&ci))w=sf_rw_write(&vol,ci,0,0,payload,sizeof(payload));
   else w=SF_DEVICE_GONE;
  }
  CHECK(r!=0||w!=(long)sizeof(payload));
  fail_at=0;mode=0;operations=0;
  recover();
  check_crash_state(&rec);
 }
 fail_at=0;mode=0;
 /* 场景 2：rename 覆盖。 */
 format_blank();
 make_file(&vol,"/from",0,A);
 make_file(&vol,"/to",0,B);
 memcpy(baseline,stable,BYTES);
 boot(&vol);
 operations=0;fail_at=0;mode=0;
 CHECK(!sf_rw_rename(&vol,"/from","/to",0));
 total=operations;CHECK(total>0);
 rng_state=0x7f4a1234u;
 for(int i=0;i<256;i++){
  char fb[32],tb[32];int fn,tn,oldstate,newstate;
  memcpy(stable,baseline,BYTES);
  boot(&vol);
  operations=0;fail_at=1+(int)(rng_next()%(uint32_t)total);mode=(int)(rng_next()%3u);
  CHECK(sf_rw_rename(&vol,"/from","/to",0)!=0);
  fail_at=0;mode=0;operations=0;
  recover();
  fn=read_text(&rec,"/from",0,fb,sizeof(fb));
  tn=read_text(&rec,"/to",0,tb,sizeof(tb));
  oldstate=fn==(int)strlen(A)&&!memcmp(fb,A,strlen(A))&&
           tn==(int)strlen(B)&&!memcmp(tb,B,strlen(B));
  newstate=fn==SF_NOT_FOUND&&tn==(int)strlen(A)&&!memcmp(tb,A,strlen(A));
  CHECK(oldstate||newstate);
 }
 fail_at=0;mode=0;
}

int main(void){
 for(size_t i=0;i<sizeof(payload);i++)payload[i]=(uint8_t)(i*13u+5u);
 for(size_t i=0;i<sizeof(pattern);i++)pattern[i]=(uint8_t)(i*7u+3u);
 test_format_mount();
 test_files();
 test_dirs();
 test_permissions();
 test_quota();
 test_rename();
 test_link();
 test_chmod();
 test_chown();
 test_fsck();
 crash_create_write();
 crash_rename();
 crash_random();
 printf("SFRW PASS: %d checks, %u blocks, crash boundaries incl. 256x2 seeded random samples\n",checks,BLOCKS);
 return 0;
}
