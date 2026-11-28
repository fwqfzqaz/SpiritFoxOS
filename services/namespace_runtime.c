#include "namespace_runtime.h"
static struct {SfNsControl control;SfReadFs*fs;Sfrw*rw;unsigned rw_writable;SfNsBrokerExchange exchange;void*context;uint64_t sequence;unsigned faulted;} service;
#define SF_NS_RW_BIT 0x80000000u
/* UINT32_MAX is the legacy broker-backed NODE_DISK sentinel and also has the
 * high bit set, so it must never be classified as an SFRWFS1 node. */
#define SF_NS_IS_RW(index) ((index)!=UINT32_MAX&&((index)&SF_NS_RW_BIT))
static int64_t bad(void){service.faulted=1;return SF_BAD_FORMAT;}
static int64_t broker(SfMessage*m,unsigned op){
 if(!service.exchange||service.faulted)return SF_DEVICE_GONE;
 if(service.sequence==UINT64_MAX){service.faulted=1;return SF_NO_SPACE;}
 uint64_t id=++service.sequence,handle=m->handle,offset=m->offset;uint32_t mode=m->mode,requested=m->length;
 m->h=(SfIpcHeader){40,1,0,op,0,id,sizeof(*m)-40,0,0};
 int64_t from=service.exchange(service.context,m);if(from<0){service.faulted=1;return from;}
 if(from!=1||m->h.flags!=1||m->h.request_id!=id||m->h.opcode!=op||m->h.size!=40||m->h.protocol_major!=1||
    m->h.protocol_minor||m->h.payload_bytes!=sizeof(*m)-40||m->h.cap_count||m->h.deadline_ns||
    m->handle!=handle||m->offset!=offset||m->mode!=mode||m->length>sizeof(m->data))return bad();
 if(m->status<0){if(m->length||m->status<INT32_MIN)return bad();return m->status;}
 if(op==OP_NS_CB_READ||op==OP_NS_CB_PROJECTION){if((uint64_t)m->status>requested||m->length!=(uint64_t)m->status)return bad();}
 else if(op==OP_NS_CB_WRITE||op==OP_NS_CB_CONSOLE){if(m->length||(uint64_t)m->status>requested)return bad();}
 else if(m->length||m->status)return bad();
 return m->status;
}
static long console(const void*data,size_t bytes){
 if(bytes>SF_MSG_BYTES)return SF_INVALID;SfMessage m={0};m.length=(uint32_t)bytes;memcpy(m.data,data,bytes);return(long)broker(&m,OP_NS_CB_CONSOLE);
}
static long callback_read(unsigned op,unsigned index,uint64_t offset,void*out,size_t bytes){
 if(bytes>SF_MSG_BYTES)return SF_INVALID;SfMessage m={0};m.mode=index;m.offset=offset;m.length=(uint32_t)bytes;
 int64_t r=broker(&m,op);if(r>=0)memcpy(out,m.data,(size_t)r);return(long)r;
}
static long read(unsigned index,uint64_t offset,void*out,size_t bytes){
 if(SF_NS_IS_RW(index)){
  Sfrw*rw=service.rw;if(!rw||!rw->mounted)return SF_DEVICE_GONE;
  uint32_t ino=index&~SF_NS_RW_BIT;if(!ino||ino>=SF_RW_MAX_INODES)return SF_INVALID;
  return sf_rw_read(rw,ino,0,offset,out,bytes);
 }
 if(index==UINT32_MAX)return callback_read(OP_NS_CB_READ,index,offset,out,bytes);
 if(!service.fs||!service.fs->mounted)return SF_DEVICE_GONE;
 SfMessage m={0};int64_t r=broker(&m,OP_NS_CB_STORAGE_LIVE);if(r)return(long)r;
 return sf_readfs_read(service.fs,index,offset,out,bytes);
}
static long write(unsigned index,uint64_t offset,const void*data,size_t bytes){
 if(SF_NS_IS_RW(index)){
  Sfrw*rw=service.rw;if(!rw||!rw->mounted)return SF_DEVICE_GONE;
  if(!service.rw_writable)return SF_READ_ONLY;
  uint32_t ino=index&~SF_NS_RW_BIT;if(!ino||ino>=SF_RW_MAX_INODES)return SF_INVALID;
  return sf_rw_write(rw,ino,0,offset,data,bytes);
 }
 if(index!=UINT32_MAX)return SF_READ_ONLY;if(bytes>SF_MSG_BYTES)return SF_INVALID;
 SfMessage m={0};m.mode=index;m.offset=offset;m.length=(uint32_t)bytes;memcpy(m.data,data,bytes);return(long)broker(&m,OP_NS_CB_WRITE);
}
static int truncate(unsigned index,uint64_t bytes){
 if(SF_NS_IS_RW(index)){
  Sfrw*rw=service.rw;if(!rw||!rw->mounted)return SF_DEVICE_GONE;
  if(!service.rw_writable)return SF_READ_ONLY;
  uint32_t ino=index&~SF_NS_RW_BIT;if(!ino||ino>=SF_RW_MAX_INODES)return SF_INVALID;
  return sf_rw_truncate(rw,ino,0,bytes);
 }
 if(index!=UINT32_MAX)return SF_READ_ONLY;SfMessage m={0};m.mode=index;m.offset=bytes;return(int)broker(&m,OP_NS_CB_TRUNCATE);
}
static long projection(unsigned index,uint64_t offset,void*out,size_t bytes){return callback_read(OP_NS_CB_PROJECTION,index,offset,out,bytes);}
static int mount_open(unsigned tag){SfMessage m={0};m.handle=tag;return(int)broker(&m,OP_NS_CB_MOUNT_OPEN);}
static void mount_close(unsigned tag){SfMessage m={0};m.handle=tag;if(broker(&m,OP_NS_CB_MOUNT_CLOSE))service.faulted=1;}
void sf_nsv_init(SfReadFs*fs,SfNsBrokerExchange exchange,void*context){
 memset(&service,0,sizeof(service));service.fs=fs;service.exchange=exchange;service.context=context;
 const SfNsBackends b={console,read,write,truncate,mount_open,mount_close,projection};sf_nsr_init(&service.control,sf_ns_default(),&b);
}
int sf_nsv_faulted(void){return service.faulted!=0;}
int sf_nsv_ready(void){return service.control.ready!=0;}
int64_t sf_nsv_dispatch(uint32_t caller,SfMessage*m){
 if(service.faulted){m->status=SF_DEVICE_GONE;m->length=0;return m->status;}
 return sf_nsr_dispatch(&service.control,caller,m);
}
static int entry(SfMountEntry*entries,unsigned*count,const char*path,unsigned type,unsigned external,uint32_t bytes,unsigned readonly){
 for(unsigned i=0;i<*count;i++)if(!strcmp(entries[i].path,path))return entries[i].type==NODE_DIR&&type==NODE_DIR?0:SF_BUSY;
 if(*count==SF_NS_NODE_MAX||strlen(path)>=96)return SF_NO_SPACE;
 SfMountEntry*e=&entries[(*count)++];memset(e,0,sizeof(*e));memcpy(e->path,path,strlen(path)+1);
 e->type=type;e->readonly=readonly?1:0;e->external=external;e->size=bytes;return 0;
}
int sf_nsv_publish_disk(uint64_t immutable_limit){
 SfReadFs*fs=service.fs;if(!service.control.ready||!fs||!fs->mounted)return SF_DEVICE_GONE;
 if(fs->count>SF_READFS_MAX_ENTRIES||!fs->capacity||immutable_limit>fs->capacity)return SF_INVALID;
 uint64_t limit=immutable_limit?immutable_limit:fs->capacity;
 SfMountEntry entries[SF_NS_NODE_MAX];unsigned count=0;int r=entry(entries,&count,"/disk",NODE_DIR,0,0,1);if(r)return r;
 for(unsigned i=0;i<fs->count;i++){
  SfReadFile*f=&fs->files[i];size_t n=0;while(n<sizeof(f->path)&&f->path[n])n++;
  if(!n||n==sizeof(f->path)||f->path[0]!='/'||f->bytes>UINT32_MAX||f->offset>limit||f->bytes>limit-f->offset)return SF_BAD_FORMAT;
  char path[96]="/disk";memcpy(path+5,f->path,n+1);
  for(char*p=path+6;*p;p++)if(*p=='/'){*p=0;r=entry(entries,&count,path,NODE_DIR,0,0,1);*p='/';if(r)return r;}
  r=entry(entries,&count,path,NODE_DISK,i,(uint32_t)f->bytes,1);if(r)return r;
 }
 return sf_ns_mount_publish(service.control.ns,entries,count,1);
}
int sf_nsv_publish_rw(Sfrw*rw,int writable){
 if(!service.control.ready||!rw||!rw->mounted)return SF_DEVICE_GONE;
 static struct {uint32_t inode;char path[96];}queue[SF_NS_NODE_MAX];
 SfMountEntry entries[SF_NS_NODE_MAX];unsigned count=0;
 int r=entry(entries,&count,"/disk",NODE_DIR,0,0,1);if(r)return r;
 unsigned head=0,tail=0;queue[tail].inode=SF_RW_ROOT;memcpy(queue[tail].path,"/disk",6);tail++;
 while(head<tail){
  uint32_t dir=queue[head].inode;const char*base=queue[head].path;head++;
  for(uint32_t index=0;;index++){
   char name[SF_RW_NAME_MAX+1];uint32_t child=0;uint8_t type=0;
   int rc=sf_rw_readdir(rw,dir,0,index,name,&child,&type);
   if(rc==SF_NOT_FOUND)break;
   if(rc)return rc==SF_DEVICE_GONE?SF_DEVICE_GONE:SF_BAD_FORMAT;
   if(!child||child>=SF_RW_MAX_INODES)return SF_BAD_FORMAT;
   if(!strcmp(name,".")||!strcmp(name,".."))continue;
   if(type!=SF_RW_ENTRY_FILE&&type!=SF_RW_ENTRY_DIR)return SF_BAD_FORMAT;
   size_t bl=strlen(base),nl=strlen(name);
   if(bl+nl+2>sizeof(entries[0].path))return SF_BAD_FORMAT;
   char path[96];memcpy(path,base,bl);path[bl]='/';memcpy(path+bl+1,name,nl+1);
   uint32_t bytes=0;unsigned ro=1;
   if(type==SF_RW_ENTRY_FILE){
    SfrwStat st;int sr=sf_rw_stat(rw,child,&st);if(sr)return sr;
    if(st.size>UINT32_MAX)return SF_NO_SPACE;
    bytes=(uint32_t)st.size;ro=writable?0:1;
   }
   r=entry(entries,&count,path,type==SF_RW_ENTRY_DIR?NODE_DIR:NODE_DISK,SF_NS_RW_BIT|child,bytes,ro);
   if(r)return r;
   if(type==SF_RW_ENTRY_DIR){
    if(tail==SF_NS_NODE_MAX)return SF_NO_SPACE;
    queue[tail].inode=child;memcpy(queue[tail].path,path,bl+nl+2);tail++;
   }
  }
 }
 return sf_ns_mount_publish(service.control.ns,entries,count,1);
}
int sf_nsv_attach_rw(Sfrw*rw,int writable){
 if(!rw||!rw->mounted)return SF_INVALID;
 service.rw=rw;service.rw_writable=writable!=0;
 /* The root frontend advertises only the readfs/broker read backend. Writes
  * to this volume are served inside this process, so install them directly. */
 sf_ns_external(service.control.ns,service.control.backends.read,service.control.backends.write,service.control.backends.truncate);
 int r=sf_nsv_publish_rw(rw,writable);
 if(r){service.rw=0;service.rw_writable=0;}
 return r;
}
void sf_nsv_detach_rw(void){service.rw=0;service.rw_writable=0;}
