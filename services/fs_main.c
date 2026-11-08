/* sf-fs.sfmod: the generic read-only disk filesystem service.
 * Format dispatch and file reads live here. The process never receives the
 * virtio-blk endpoints directly: sf-init stays the sole receiver of block
 * replies and answers bounded OP_FS_BLOCK_READ requests through its broker. */
#include "libsf.h"
#include "fs_format.h"
static uint32_t broker_ep,broker_reply,init_pid;
static uint64_t capacity,sequence=0x500000000ull;
static SfReadFs fs;
static uint8_t verify[SF_READFS_FILE_MAX];
static int broker_call(SfMessage*m){
 if(!broker_ep||!broker_reply||!init_pid)return SF_DEVICE_GONE;
 uint64_t id=++sequence;m->h.request_id=id;
 int r=sf_send(broker_ep,m);if(r)return r;
 long from=sf_recv(broker_reply,m,sf_deadline(30000));if(from<0)return(int)from;
 if(from!=(long)init_pid||m->h.request_id!=id)return SF_STALE_HANDLE;
 return(int)m->status;
}
static int sector_read(void*context,uint64_t lba,uint8_t out[512]){
 (void)context;if(lba>=capacity/512||!capacity)return SF_INVALID;
 for(unsigned half=0;half<512;half+=256){
  SfMessage m;sf_message(&m,OP_FS_BLOCK_READ,0);m.offset=lba*512;m.mode=half;m.length=256;
  int r=broker_call(&m);if(r)return r;if(m.length!=256)return SF_BAD_FORMAT;memcpy(out+half,m.data,256);
 }
 return 0;
}
static int check_mount(void){
 if(!fs.mounted)return SF_DEVICE_GONE;
 long n=0;for(unsigned i=0;i<fs.count;i++){n=sf_readfs_load(&fs,i,verify,sizeof(verify));if(n<0)return(int)n;}
 return 0;
}
static int dispatch(SfMessage*m){
 switch(m->h.opcode){
 case OP_FS_ATTACH_SEND:
  if(!m->handle||!m->offset)return SF_BAD_FORMAT;
  if(broker_ep)sf_cap_close(broker_ep);
  broker_ep=(uint32_t)m->handle;capacity=m->offset;return 0;
 case OP_FS_ATTACH_REPLY:
  if(!m->handle)return SF_BAD_FORMAT;
  if(broker_reply)sf_cap_close(broker_reply);
  broker_reply=(uint32_t)m->handle;return 0;
 case OP_FS_MOUNT:{
  int format=0,r;
  if(m->mode>SF_FS_FORMAT_SFRFS2)return SF_INVALID;
  r=sf_fs_mount_auto(&fs,sector_read,0,capacity,(int)m->mode,&format);if(r)return r;
  /* Mount verification reads the whole image; the reply may legitimately
   * outlive the request's own send deadline. */
  r=check_mount();if(r){fs.mounted=0;return r;}
  m->handle=fs.count;m->mode=(uint32_t)format;return 0;}
 case OP_FS_ENTRY:{
  if(!fs.mounted||m->offset>=fs.count)return SF_INVALID;
  SfReadFile*f=&fs.files[m->offset];size_t n=strlen(f->path);
  if(n>sizeof(m->data))return SF_BAD_FORMAT;
  memcpy(m->data,f->path,n);m->length=(uint32_t)n;m->handle=f->bytes;m->offset=f->offset;return 0;}
 case OP_FS_FIND:{
  if(!fs.mounted||m->length>sizeof(m->data)||(m->length&&m->data[m->length-1]))return SF_INVALID;
  int i=sf_readfs_find(&fs,(const char*)m->data);if(i<0)return i;
  m->offset=(uint64_t)i;m->handle=fs.files[i].bytes;return 0;}
 case OP_FS_READ:{
  if(!fs.mounted||m->length>sizeof(m->data))return SF_INVALID;
  long n=sf_readfs_read(&fs,m->mode,m->offset,m->data,m->length);if(n<0)return(int)n;
  m->length=(uint32_t)n;return 0;}
 case OP_FS_STOP:fs.mounted=0;return 0;
 }
 return SF_INVALID;
}
long sf_main(const SfBootArgs*a,size_t size){
 if(size!=sizeof(*a)||a->abi_major!=1||!a->service||!a->manager)return SF_ABI_MISMATCH;
 sf_log("[FS] generic disk filesystem service in ring 3\n");
 for(;;){
  SfMessage m;long from=sf_recv(a->service,&m,0);if(from<0)return from;
  if(!init_pid)init_pid=(uint32_t)from;
  /* Only the manager that spawned us may drive the service. */
  m.status=from==(long)init_pid?dispatch(&m):SF_DENIED;
  m.h.cap_count=0;m.h.deadline_ns=0; /* replies are not bound by the request deadline */
  sf_send(a->manager,&m);
 }
}
