/* sf-fs owns the disk parser and namespace/RAMFS in its private address space. */
#include "libsf.h"
#include "fs_format.h"
#include "namespace_runtime.h"
static uint32_t broker_ep,broker_reply;
static uint64_t capacity,sequence=0x500000000ull;
static SfReadFs fs;
static uint8_t verify[SF_READFS_FILE_MAX];
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
static int check_mount(void){
 if(!fs.mounted)return SF_DEVICE_GONE;
 for(unsigned i=0;i<fs.count;i++){long n=sf_readfs_load(&fs,i,verify,sizeof(verify));if(n<0)return(int)n;}
 return 0;
}
static int64_t disk_dispatch(SfMessage*m){
 switch(m->h.opcode){
 case OP_FS_ATTACH_SEND:
  if(!m->handle||m->handle>UINT32_MAX||m->h.cap_count!=1||fs.mounted)return SF_BAD_FORMAT;
  if(broker_ep)sf_cap_close(broker_ep);
  broker_ep=(uint32_t)m->handle;capacity=m->offset;return 0;
 case OP_FS_ATTACH_REPLY:
  if(!m->handle||m->handle>UINT32_MAX||m->h.cap_count!=1||fs.mounted)return SF_BAD_FORMAT;
  if(broker_reply)sf_cap_close(broker_reply);
  broker_reply=(uint32_t)m->handle;return 0;
 case OP_FS_MOUNT:{
  if(fs.mounted)return SF_BUSY;
  if(m->mode>SF_FS_FORMAT_SFRFS2||m->offset>capacity)return SF_INVALID;
  uint64_t limit=m->offset;int format=0;
  int r=sf_fs_mount_auto(&fs,sector_read,0,capacity,(int)m->mode,&format);if(r)return r;
  r=check_mount();if(!r&&sf_nsv_ready())r=sf_nsv_publish_disk(limit);
  if(r){fs.mounted=0;return r;}
  m->handle=fs.count;m->mode=(uint32_t)format;return 0;}
 case OP_FS_ENTRY:{
  if(!fs.mounted||m->offset>=fs.count)return SF_INVALID;
  SfReadFile*f=&fs.files[m->offset];size_t n=strlen(f->path);
  if(n>sizeof(m->data))return SF_BAD_FORMAT;
  memcpy(m->data,f->path,n);m->length=(uint32_t)n;m->handle=f->bytes;m->offset=f->offset;return 0;}
 case OP_FS_FIND:{
  if(!fs.mounted||!m->length||m->length>sizeof(m->data)||m->data[m->length-1])return SF_INVALID;
  for(unsigned j=0;j+1<m->length;j++)if(!m->data[j])return SF_INVALID;
  int i=sf_readfs_find(&fs,(const char*)m->data);if(i<0)return i;
  m->offset=(uint64_t)i;m->handle=fs.files[i].bytes;return 0;}
 case OP_FS_READ:{
  if(!fs.mounted||m->length>sizeof(m->data))return SF_INVALID;
  long n=sf_readfs_read(&fs,m->mode,m->offset,m->data,m->length);if(n<0)return(int)n;
  m->length=(uint32_t)n;return 0;}
 case OP_FS_STOP:fs.mounted=0;return 0;
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
 else if(m->h.opcode>=OP_NS_INIT&&m->h.opcode<=OP_NS_UNMOUNT)r=sf_nsv_dispatch(1,m);
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
