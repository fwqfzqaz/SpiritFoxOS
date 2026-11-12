#include "namespace_frontend.h"
#include "namespace_client.h"
/* Root-side representation. The large namespace tables exist only at the
 * service. This adapter is linked instead of namespace.c, never with it. */
struct SfNamespace {
 SfNsClient client;
 long(*console)(const void*,size_t);
 SfNsRead read,projection;
 long(*write)(unsigned,uint64_t,const void*,size_t);
 int(*truncate)(unsigned,uint64_t);
 int(*mount_open)(unsigned);
 void(*mount_close)(unsigned);
};
static SfNamespace frontend;
SfNamespace*sf_ns_default(void){return &frontend;}
static void lost(void*context){(void)context;sf_init_namespace_lost();}
void sf_ns_peer_lost(void){sf_nsc_lost(&frontend.client);}
int sf_ns_client_healthy(void){return frontend.client.ready!=0;}
void sf_ns_init(SfNamespace*n,long(*console)(const void*,size_t)){
 n->console=console;n->read=n->projection=0;n->write=0;n->truncate=0;n->mount_open=0;n->mount_close=0;
 int pid=sf_storage_namespace_start();if(pid<0){sf_nsc_lost(&n->client);return;}
 sf_nsc_connect(&n->client,(uint32_t)pid,sf_storage_namespace_exchange,n,lost,console?SF_NS_B_CONSOLE:0);
}
static void backends(SfNamespace*n){
 uint32_t mask=(n->console?SF_NS_B_CONSOLE:0)|(n->read?SF_NS_B_READ:0)|(n->write?SF_NS_B_WRITE:0)|
  (n->truncate?SF_NS_B_TRUNCATE:0)|(n->mount_open?SF_NS_B_MOUNT_OPEN:0)|
  (n->mount_close?SF_NS_B_MOUNT_CLOSE:0)|(n->projection?SF_NS_B_PROJECTION:0);
 if(n->client.ready)sf_nsc_backends(&n->client,mask);
}
SfNsRead sf_ns_external(SfNamespace*n,SfNsRead read,long(*write)(unsigned,uint64_t,const void*,size_t),int(*truncate)(unsigned,uint64_t)){
 SfNsRead old=n->read;n->read=read;n->write=write;n->truncate=truncate;backends(n);return old;
}
void sf_ns_mount_hooks(SfNamespace*n,int(*open)(unsigned),void(*close)(unsigned)){n->mount_open=open;n->mount_close=close;backends(n);}
void sf_ns_projection_backend(SfNamespace*n,SfNsRead read){n->projection=read;backends(n);}
int sf_ns_create_owned(SfNamespace*n,const char*p,unsigned type,int readonly,uint32_t uid,uint32_t gid,unsigned mode){return sf_nsc_create(&n->client,p,type,readonly,uid,gid,mode);}
int sf_ns_create(SfNamespace*n,const char*p,unsigned type,int readonly){return sf_ns_create_owned(n,p,type,readonly,0,0,type==NODE_DIR?0755:0644);}
int sf_ns_assign(SfNamespace*n,const char*p,uint32_t uid,uint32_t gid,unsigned mode){return sf_nsc_assign(&n->client,p,uid,gid,mode);}
int sf_ns_project(SfNamespace*n,const char*p,unsigned id){return sf_nsc_project(&n->client,p,id);}
int sf_ns_protect(SfNamespace*n,const char*p){return sf_nsc_protect(&n->client,p);}
int sf_ns_mount_check(SfNamespace*n,const char*p){return sf_nsc_mount_check(&n->client,p);}
int sf_ns_mount_publish(SfNamespace*n,const SfMountEntry*entries,unsigned count,unsigned tag){return sf_nsc_mount_publish(&n->client,entries,count,tag);}
int sf_ns_open_as(SfNamespace*n,uint32_t owner,const char*root,const char*p,unsigned mode,uint32_t uid,uint32_t gid){return sf_nsc_open(&n->client,owner,root,p,mode,uid,gid);}
int sf_ns_open(SfNamespace*n,uint32_t owner,const char*root,const char*p,unsigned mode){return sf_ns_open_as(n,owner,root,p,mode,0,0);}
int sf_ns_openat_as(SfNamespace*n,uint32_t owner,uint32_t parent,const char*p,unsigned mode,uint32_t uid,uint32_t gid){return sf_nsc_openat(&n->client,owner,parent,p,mode,uid,gid);}
int sf_ns_openat(SfNamespace*n,uint32_t owner,uint32_t parent,const char*p,unsigned mode){return sf_ns_openat_as(n,owner,parent,p,mode,0,0);}
int sf_ns_unlink_as(SfNamespace*n,uint32_t owner,uint32_t h,const char*p,uint32_t uid,uint32_t gid){return sf_nsc_remove(&n->client,owner,h,p,uid,gid,0);}
int sf_ns_unlink(SfNamespace*n,uint32_t owner,uint32_t h,const char*p){return sf_ns_unlink_as(n,owner,h,p,0,0);}
int sf_ns_rmdir_as(SfNamespace*n,uint32_t owner,uint32_t h,const char*p,uint32_t uid,uint32_t gid){return sf_nsc_remove(&n->client,owner,h,p,uid,gid,1);}
int sf_ns_rmdir(SfNamespace*n,uint32_t owner,uint32_t h,const char*p){return sf_ns_rmdir_as(n,owner,h,p,0,0);}
int sf_ns_stat(SfNamespace*n,uint32_t owner,uint32_t h,SfFileStat*out){return sf_nsc_stat(&n->client,owner,h,out);}
int sf_ns_dup(SfNamespace*n,uint32_t owner,uint32_t h){return sf_nsc_dup(&n->client,owner,h);}
int sf_ns_close(SfNamespace*n,uint32_t owner,uint32_t h){return sf_nsc_close(&n->client,owner,h);}
long sf_ns_read(SfNamespace*n,uint32_t owner,uint32_t h,void*out,size_t bytes){return(long)sf_nsc_read(&n->client,owner,h,out,bytes,0,0);}
long sf_ns_pread(SfNamespace*n,uint32_t owner,uint32_t h,void*out,size_t bytes,uint64_t offset){return(long)sf_nsc_read(&n->client,owner,h,out,bytes,offset,1);}
long sf_ns_write(SfNamespace*n,uint32_t owner,uint32_t h,const void*data,size_t bytes){return(long)sf_nsc_write(&n->client,owner,h,data,bytes);}
int sf_ns_truncate(SfNamespace*n,uint32_t owner,uint32_t h,uint64_t bytes){return sf_nsc_truncate(&n->client,owner,h,bytes);}
int64_t sf_ns_seek(SfNamespace*n,uint32_t owner,uint32_t h,uint64_t offset){return sf_nsc_seek(&n->client,owner,h,offset);}
long sf_ns_poll(SfNamespace*n,uint32_t owner,uint32_t h){return(long)sf_nsc_poll(&n->client,owner,h);}
long sf_ns_enumerate(SfNamespace*n,uint32_t owner,uint32_t h,char*out,size_t bytes){return(long)sf_nsc_enumerate(&n->client,owner,h,out,bytes);}
long sf_ns_enumerate_at(SfNamespace*n,uint32_t owner,uint32_t h,uint32_t index,char*out,size_t bytes){return(long)sf_nsc_enumerate_at(&n->client,owner,h,index,out,bytes);}
void sf_ns_reap(SfNamespace*n,uint32_t owner){sf_nsc_reap(&n->client,owner);}
int sf_ns_handoff(SfNamespace*n,uint32_t owner,uint32_t h,uint32_t next){return sf_nsc_handoff(&n->client,owner,h,next);}
int sf_ns_unmount(SfNamespace*n,unsigned tag){return sf_nsc_unmount(&n->client,tag);}
static int64_t callback(SfNamespace*n,SfMessage*m){
 if(m->handle||m->length>sizeof(m->data))return SF_INVALID;
 switch(m->h.opcode){
 case OP_NS_CB_CONSOLE:
  if(m->mode||m->offset)return SF_INVALID;
  return n->console?n->console(m->data,m->length):SF_DEVICE_GONE;
 case OP_NS_CB_READ:
  if(m->mode!=UINT32_MAX)return SF_DENIED;
  return n->read?n->read(m->mode,m->offset,m->data,m->length):SF_DEVICE_GONE;
 case OP_NS_CB_WRITE:
  if(m->mode!=UINT32_MAX)return SF_DENIED;
  return n->write?n->write(m->mode,m->offset,m->data,m->length):SF_DEVICE_GONE;
 case OP_NS_CB_TRUNCATE:
  if(m->mode!=UINT32_MAX||m->length)return SF_INVALID;
  return n->truncate?n->truncate(m->mode,m->offset):SF_DEVICE_GONE;
 case OP_NS_CB_PROJECTION:
  return n->projection?n->projection(m->mode,m->offset,m->data,m->length):SF_DEVICE_GONE;
 case OP_NS_CB_STORAGE_LIVE:
  if(m->mode||m->offset||m->length)return SF_INVALID;
  return sf_storage_namespace_disk_live();
 }
 return SF_INVALID;
}
int64_t sf_ns_client_callback(uint32_t from,SfMessage*m){
 SfNamespace*n=&frontend;int64_t r;uint32_t requested=m->length,op=m->h.opcode;
 if(!n->client.ready||from!=n->client.peer||m->h.flags!=from)r=SF_DENIED;
 else if(m->h.size!=40||m->h.protocol_major!=1||m->h.protocol_minor||m->h.payload_bytes!=sizeof(*m)-40||
    m->h.cap_count||!m->h.request_id||m->status||m->length>sizeof(m->data))r=SF_INVALID;
 else if(op==OP_NS_CB_MOUNT_OPEN||op==OP_NS_CB_MOUNT_CLOSE){
  if(!m->handle||m->handle>UINT32_MAX||m->mode||m->offset||m->length)r=SF_INVALID;
  else if(op==OP_NS_CB_MOUNT_OPEN)r=n->mount_open?n->mount_open((unsigned)m->handle):SF_DEVICE_GONE;
  else{r=n->mount_close?0:SF_DEVICE_GONE;if(n->mount_close)n->mount_close((unsigned)m->handle);if(!n->client.ready)r=SF_DEVICE_GONE;}
 }else r=callback(n,m);
 if(r>=0&&(op==OP_NS_CB_CONSOLE||op==OP_NS_CB_READ||op==OP_NS_CB_WRITE||op==OP_NS_CB_PROJECTION)&&(uint64_t)r>requested)r=SF_BAD_FORMAT;
 m->length=r>=0&&(op==OP_NS_CB_READ||op==OP_NS_CB_PROJECTION)?(uint32_t)r:0;m->status=r;return r;
}
