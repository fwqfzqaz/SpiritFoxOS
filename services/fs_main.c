/* sf-fs owns the disk parser and namespace/RAMFS in its private address space. */
#include "libsf.h"
#include "fs_format.h"
#include "sfrw.h"
#include "fs_rw.h"
#include "namespace_runtime.h"
static uint32_t broker_ep,broker_reply;
static uint64_t capacity,sequence=0x500000000ull;
static SfReadFs fs;
/* 常驻的只有当前卷状态；只读校验缓冲按挂载时的最大文件临时映射，
 * 避免为从不校验大文件的进程常驻 1 MiB。 */
static Sfrw rw;
static int rw_mounted;
static int64_t exchange(void*context,SfMessage*m){
 (void)context;if(!broker_ep||!broker_reply)return SF_DEVICE_GONE;
 int r=sf_send(broker_ep,m);if(r)return r;
 long from=sf_recv(broker_reply,m,sf_deadline(30000));
 if(from>=0&&m->h.cap_count==1)sf_cap_close(m->handle);
 return from;
}
static int broker_call(SfMessage*m){
 if(sequence==UINT64_MAX)return SF_NO_SPACE;
 uint64_t id=++sequence;unsigned opcode=m->h.opcode;m->h.request_id=id;
 int64_t from=exchange(0,m);if(from<0)return(int)from;
 if(from!=1||m->h.flags!=1||m->h.request_id!=id||m->h.opcode!=opcode||
    m->h.size!=40||m->h.protocol_major!=1||m->h.protocol_minor||
    m->h.payload_bytes!=sizeof(*m)-40||m->h.cap_count||m->h.deadline_ns)return SF_STALE_HANDLE;
 if(m->status>0||m->status<SF_VERSION_REJECTED)return SF_BAD_FORMAT;
 return(int)m->status;
}
static int sector_read(void*context,uint64_t lba,uint8_t out[512]){
 (void)context;if(!capacity||lba>=capacity/512)return SF_INVALID;
 for(unsigned half=0;half<512;half+=256){
  SfMessage m;sf_message(&m,OP_FS_BLOCK_READ,0);m.offset=lba*512;m.mode=half;m.length=256;
  int r=broker_call(&m);if(r)return r;
  if(m.offset!=lba*512||m.mode!=half||m.length!=256)return SF_BAD_FORMAT;
  memcpy(out+half,m.data,256);
 }return 0;
}
static int sector_write(void*context,uint64_t lba,const uint8_t in[512]){
 (void)context;if(!capacity||lba>=capacity/512)return SF_INVALID;
 for(unsigned half=0;half<512;half+=256){
  SfMessage m;sf_message(&m,OP_FS_BLOCK_WRITE,0);m.offset=lba*512;m.mode=half;m.length=256;
  memcpy(m.data,in+half,256);
  int r=broker_call(&m);if(r)return r;
  if(m.offset!=lba*512||m.mode!=half||m.length)return SF_BAD_FORMAT;
 }return 0;
}
static int sector_flush(void*context){
 (void)context;SfMessage m;sf_message(&m,OP_FS_BLOCK_FLUSH,0);
 int r=broker_call(&m);if(r)return r;
 return (m.offset||m.mode||m.length)?SF_BAD_FORMAT:0;
}
static SfRwIo rw_io={sector_read,sector_write,sector_flush,0};
static int rw_present(void){
 uint8_t head[512];if(sector_read(0,0,head))return 0;
 return !memcmp(head,"SFRWFS1\0",8);
}
static int check_mount(void){
 if(!fs.mounted)return SF_DEVICE_GONE;
 uint64_t need=0;
 for(unsigned i=0;i<fs.count;i++)if(fs.files[i].bytes>need)need=fs.files[i].bytes;
 if(!need)return 0;
 uint64_t bytes=(need+4095u)&~4095ull;
 long buffer=sf_mmap(0,bytes);if(buffer<0)return(int)buffer;
 int result=0;
 for(unsigned i=0;i<fs.count;i++){long n=sf_readfs_load(&fs,i,(void*)buffer,(size_t)need);if(n<0){result=(int)n;break;}}
 sf_munmap((uint64_t)buffer,bytes);
 return result;
}
static int64_t disk_dispatch(SfMessage*m){
 switch(m->h.opcode){
 case OP_FS_ATTACH_SEND:
  if(!m->handle||m->handle>UINT32_MAX||m->h.cap_count!=1||fs.mounted||rw_mounted)return SF_BAD_FORMAT;
  if(broker_ep)sf_cap_close(broker_ep);
  broker_ep=(uint32_t)m->handle;capacity=m->offset;return 0;
 case OP_FS_ATTACH_REPLY:
  if(!m->handle||m->handle>UINT32_MAX||m->h.cap_count!=1||fs.mounted||rw_mounted)return SF_BAD_FORMAT;
  if(broker_reply)sf_cap_close(broker_reply);
  broker_reply=(uint32_t)m->handle;return 0;
 case OP_FS_MOUNT:{
  if(fs.mounted||rw_mounted)return SF_BUSY;
  if(m->offset>capacity)return SF_INVALID;
  if(rw_present()){
   if(m->mode&&m->mode!=SF_FS_FORMAT_SFRWFS1)return SF_BAD_FORMAT;
   SfrwDev dev=sf_rw_io_dev(&rw_io);int r=sf_rw_mount(&rw,dev);
   if(r){SfckReport report;r=sf_rw_fsck(&rw,dev,&report);if(!r)r=sf_rw_mount(&rw,dev);}
   if(r)return r;
   r=sf_nsv_attach_rw(&rw,m->handle!=0);
   if(r){rw.mounted=0;return r;}
   rw_mounted=1;m->handle=0;m->mode=SF_FS_FORMAT_SFRWFS1;m->length=0;return 0;
  }
  if(m->mode>SF_FS_FORMAT_SFRFS2)return SF_INVALID;
  uint64_t limit=m->offset;int format=0;
  int r=sf_fs_mount_auto(&fs,sector_read,0,capacity,(int)m->mode,&format);if(r)return r;
  r=check_mount();if(!r&&sf_nsv_ready())r=sf_nsv_publish_disk(limit);
  if(r){fs.mounted=0;return r;}
  m->handle=fs.count;m->mode=(uint32_t)format;return 0;}
 case OP_FS_FORMAT:{
  if(fs.mounted||rw_mounted)return SF_BUSY;
  if(!capacity||capacity%SF_RW_BLOCK_BYTES)return SF_INVALID;
  uint32_t blocks=(uint32_t)(capacity/SF_RW_BLOCK_BYTES);
  if(blocks<SF_RW_MIN_BLOCKS||blocks>SF_RW_MAX_BLOCKS)return SF_NO_SPACE;
  uint8_t head[512];int r=sector_read(0,0,head);if(r)return r;
  for(unsigned i=0;i<sizeof(head);i++)if(head[i])return SF_INVALID;
  SfrwDev dev=sf_rw_io_dev(&rw_io);SfrwFormatOptions options={blocks,0,0};
  r=sf_rw_format(&rw,dev,&options);if(r)return r;
  r=sf_rw_mount(&rw,dev);if(!r)r=sf_rw_mkdir(&rw,"/etc",0,0755);
  if(!r)r=sf_rw_create(&rw,"/etc/readme.txt",0,0644);
  uint32_t ino=0;
  if(!r)r=sf_rw_open(&rw,"/etc/readme.txt",0,SF_RW_WRITE,&ino);
  static const char seed[]="SpiritFoxOS SFRWFS1 volume\n";
  if(!r&&sf_rw_write(&rw,ino,0,0,seed,sizeof(seed)-1)!=(long)(sizeof(seed)-1))r=SF_COMMIT_UNCERTAIN;
  if(!r)r=sf_rw_fsync(&rw);
  rw.mounted=0;
  if(r)return r;
  m->handle=0;m->mode=SF_FS_FORMAT_SFRWFS1;m->length=0;return 0;}
 case OP_FS_ENTRY:{
  if(rw_mounted||!fs.mounted||m->offset>=fs.count)return SF_INVALID;
  SfReadFile*f=&fs.files[m->offset];size_t n=strlen(f->path);
  if(n>sizeof(m->data))return SF_BAD_FORMAT;
  memcpy(m->data,f->path,n);m->length=(uint32_t)n;m->handle=f->bytes;m->offset=f->offset;return 0;}
 case OP_FS_FIND:{
  if(rw_mounted||!fs.mounted||!m->length||m->length>sizeof(m->data)||m->data[m->length-1])return SF_INVALID;
  for(unsigned j=0;j+1<m->length;j++)if(!m->data[j])return SF_INVALID;
  int i=sf_readfs_find(&fs,(const char*)m->data);if(i<0)return i;
  m->offset=(uint64_t)i;m->handle=fs.files[i].bytes;return 0;}
 case OP_FS_READ:{
  if(rw_mounted||!fs.mounted||m->length>sizeof(m->data))return SF_INVALID;
  long n=sf_readfs_read(&fs,m->mode,m->offset,m->data,m->length);if(n<0)return(int)n;
  m->length=(uint32_t)n;return 0;}
 case OP_FS_STOP:
  if(rw_mounted){int r=sf_rw_fsync(&rw);sf_nsv_detach_rw();rw.mounted=0;rw_mounted=0;return r;}
  fs.mounted=0;return 0;
 }return SF_INVALID;
}
/* Actual process receive sender is pinned to PID 1, including the first
 * message. An attacker cannot claim this service by racing manager attach. */
static int64_t process_request(int64_t from,SfMessage*m){
 int attach=m->h.opcode==OP_FS_ATTACH_SEND||m->h.opcode==OP_FS_ATTACH_REPLY;
 int64_t r;
 if(from!=1)r=SF_DENIED;
 else if(m->h.size!=40||m->h.protocol_major!=1||m->h.protocol_minor||
         m->h.flags!=1||m->h.payload_bytes!=sizeof(*m)-40||m->h.cap_count>(unsigned)attach)r=SF_INVALID;
 else if(m->h.opcode>=OP_NS_INIT&&m->h.opcode<=OP_NS_RENAME)r=sf_nsv_dispatch(1,m);
 else r=disk_dispatch(m);
 if(m->h.cap_count==1&&(!attach||r))sf_cap_close(m->handle);
 m->status=r;m->h.cap_count=0;m->h.deadline_ns=0;return r;
}
long sf_main(const SfBootArgs*a,size_t size){
 if(!a||size!=sizeof(*a)||a->abi_major!=1||!a->service||!a->manager)return SF_ABI_MISMATCH;
 sf_nsv_init(&fs,exchange,0);sf_log("[FS] disk and namespace service in ring 3\n");
 for(;;){
  SfMessage m;long from=sf_recv(a->service,&m,0);if(from<0)return from;
  process_request(from,&m);
  /* A lost mount-close callback has an uncertain Root ledger effect. Exit
   * before any success reply so confirmed-dead recovery can reconcile it. */
  if(sf_nsv_faulted())return SF_DEVICE_GONE;
  long sent=sf_send(a->manager,&m);if(sent<0)return sent;
 }
}
