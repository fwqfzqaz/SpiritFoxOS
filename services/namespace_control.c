#include "namespace_control.h"
static uint32_t get32(const uint8_t*p){return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static const uint8_t*nul(const uint8_t*p,size_t n){for(size_t i=0;i<n;i++)if(!p[i])return p+i;return 0;}
static int zero(const SfMessage*m){return !m->handle&&!m->offset&&!m->length;}
static int handle(const SfMessage*m){return m->handle&&m->handle<=UINT32_MAX;}
static int path(const SfMessage*m,unsigned paths,uint32_t v[4],const char**a,const char**b){
 if(m->length<17||m->length>16+paths*96)return SF_INVALID;
 for(unsigned i=0;i<4;i++)v[i]=get32(m->data+i*4);
 const uint8_t*p=m->data+16;size_t remaining=m->length-16;
 const uint8_t*end=nul(p,remaining);if(!end||end==p||end-p>95)return SF_INVALID;
 *a=(const char*)p;size_t used=(size_t)(end-p)+1;
 if(paths==1)return used==remaining?0:SF_INVALID;
 p+=used;remaining-=used;if(!remaining)return SF_INVALID;
 end=nul(p,remaining);if(!end||end==p||end-p>95||(size_t)(end-p)+1!=remaining)return SF_INVALID;
 *b=(const char*)p;return 0;
}
static int backend(SfNsControl*s,uint32_t mask){
 const SfNsBackends*b=&s->backends;
 if(mask&~SF_NS_B_ALL)return SF_INVALID;
 if(((mask&SF_NS_B_CONSOLE)&&!b->console)||((mask&SF_NS_B_READ)&&!b->read)||
    ((mask&SF_NS_B_WRITE)&&!b->write)||((mask&SF_NS_B_TRUNCATE)&&!b->truncate)||
    ((mask&SF_NS_B_MOUNT_OPEN)&&!b->mount_open)||((mask&SF_NS_B_MOUNT_CLOSE)&&!b->mount_close)||
    ((mask&SF_NS_B_PROJECTION)&&!b->projection))return SF_INVALID;
 return 0;
}
static void apply_backend(SfNsControl*s,uint32_t mask){
 const SfNsBackends*b=&s->backends;
 sf_ns_external(s->ns,mask&SF_NS_B_READ?b->read:0,mask&SF_NS_B_WRITE?b->write:0,mask&SF_NS_B_TRUNCATE?b->truncate:0);
 sf_ns_mount_hooks(s->ns,mask&SF_NS_B_MOUNT_OPEN?b->mount_open:0,mask&SF_NS_B_MOUNT_CLOSE?b->mount_close:0);
 sf_ns_projection_backend(s->ns,mask&SF_NS_B_PROJECTION?b->projection:0);
}
void sf_nsr_init(SfNsControl*s,SfNamespace*ns,const SfNsBackends*b){
 memset(s,0,sizeof(*s));s->ns=ns;if(b)s->backends=*b;
}
static int64_t dispatch(SfNsControl*s,SfMessage*m){
 uint32_t v[4];const char*a=0,*b=0;int r;
 if(m->h.opcode==OP_NS_INIT){
  if(!zero(m)||(r=backend(s,m->mode)))return SF_INVALID;
  if(s->ready)return SF_BUSY;
  sf_ns_init(s->ns,m->mode&SF_NS_B_CONSOLE?s->backends.console:0);
  apply_backend(s,m->mode);s->mask=m->mode;s->ready=1;return 0;
 }
 if(!s->ready)return SF_DEVICE_GONE;
 switch(m->h.opcode){
 case OP_NS_BACKENDS:
  if(!zero(m)||(r=backend(s,m->mode)))return SF_INVALID;
  /* Console is fixed at INIT: namespace's old setter did not replace it. */
  if((s->mask^m->mode)&SF_NS_B_CONSOLE)return SF_INVALID;
  apply_backend(s,m->mode);s->mask=m->mode;return 0;
 case OP_NS_CREATE:
  if(m->handle||m->offset>1||path(m,1,v,&a,&b)||v[0])return SF_INVALID;
  return sf_ns_create_owned(s->ns,a,m->mode,(int)m->offset,v[1],v[2],v[3]);
 case OP_NS_ASSIGN:
  if(m->handle||m->offset||m->mode||path(m,1,v,&a,&b)||v[0])return SF_INVALID;
  return sf_ns_assign(s->ns,a,v[1],v[2],v[3]);
 case OP_NS_PROJECT:
 case OP_NS_PROTECT:
 case OP_NS_MOUNT_CHECK:
  if(m->handle||m->offset||path(m,1,v,&a,&b)||v[0]||v[1]||v[2]||v[3])return SF_INVALID;
  if(m->h.opcode==OP_NS_PROJECT)return sf_ns_project(s->ns,a,m->mode);
  if(m->mode)return SF_INVALID;
  return m->h.opcode==OP_NS_PROTECT?sf_ns_protect(s->ns,a):sf_ns_mount_check(s->ns,a);
 case OP_NS_OPEN:
  if(m->handle||m->offset||path(m,2,v,&a,&b)||!v[0]||v[3])return SF_INVALID;
  r=sf_ns_open_as(s->ns,v[0],a,b,m->mode,v[1],v[2]);
  if(r<0)return r;m->handle=(uint32_t)r;return 0;
 case OP_NS_OPENAT:
 case OP_NS_UNLINK:
 case OP_NS_RMDIR:
  if(!handle(m)||m->offset||path(m,1,v,&a,&b)||!v[0]||v[3])return SF_INVALID;
  if(m->h.opcode!=OP_NS_OPENAT&&m->mode)return SF_INVALID;
  if(m->h.opcode==OP_NS_UNLINK)return sf_ns_unlink_as(s->ns,v[0],(uint32_t)m->handle,a,v[1],v[2]);
  if(m->h.opcode==OP_NS_RMDIR)return sf_ns_rmdir_as(s->ns,v[0],(uint32_t)m->handle,a,v[1],v[2]);
  r=sf_ns_openat_as(s->ns,v[0],(uint32_t)m->handle,a,m->mode,v[1],v[2]);
  if(r<0)return r;m->handle=(uint32_t)r;return 0;
 case OP_NS_MOUNT_BEGIN:
  if(m->length||m->offset||!m->handle||m->handle>UINT32_MAX||!m->mode||m->mode>SF_NS_NODE_MAX)return SF_INVALID;
  if(s->count)return SF_BUSY;
  if(s->token==UINT64_MAX)return SF_NO_SPACE;
  s->count=m->mode;s->tag=(uint32_t)m->handle;s->next=0;m->offset=++s->token;return 0;
 case OP_NS_MOUNT_ENTRY:{
  if(!s->count||m->offset!=s->token)return SF_STALE_HANDLE;
  r=SF_INVALID;
  if(!m->handle&&!m->mode&&m->length>=21&&m->length<=116){
   unsigned length=m->length-20;const uint8_t*p=m->data+20;
   if(!p[length-1]&&strlen((const char*)p)+1==length&&get32(m->data)==s->next&&s->next<s->count){
    SfMountEntry*e=&s->entries[s->next];memset(e,0,sizeof(*e));
    memcpy(e->path,p,length);e->type=get32(m->data+4);e->readonly=get32(m->data+8);
    e->external=get32(m->data+12);e->size=get32(m->data+16);s->next++;return 0;
   }
  }
  s->count=s->next=0;return r;
 }
 case OP_NS_MOUNT_COMMIT:
 case OP_NS_MOUNT_ABORT:
  if(m->handle||m->mode||m->length||!m->offset||m->offset!=s->token)return SF_STALE_HANDLE;
  if(m->h.opcode==OP_NS_MOUNT_ABORT){s->count=s->next=0;return 0;}
  if(!s->count)return SF_STALE_HANDLE;
  r=s->next==s->count?sf_ns_mount_publish(s->ns,s->entries,s->count,s->tag):SF_INVALID;
  s->count=s->next=0;return r;
 case OP_NS_REAP:
  if(!zero(m)||!m->mode)return SF_INVALID;sf_ns_reap(s->ns,m->mode);return 0;
 case OP_NS_UNMOUNT:
  if(!zero(m)||!m->mode)return SF_INVALID;return sf_ns_unmount(s->ns,m->mode);
 }
 if(!handle(m))return SF_STALE_HANDLE;
 uint32_t h=(uint32_t)m->handle,owner=m->mode;long n;if(!owner)return SF_INVALID;
 switch(m->h.opcode){
 case OP_NS_HANDOFF:
  if(m->length||!m->offset||m->offset>UINT32_MAX)return SF_INVALID;
  return sf_ns_handoff(s->ns,owner,h,(uint32_t)m->offset);
 case OP_NS_READ:case OP_NS_PREAD:case OP_NS_ENUMERATE:
  if(m->h.opcode!=OP_NS_PREAD&&m->offset)return SF_INVALID;
  if(m->h.opcode==OP_NS_READ)n=sf_ns_read(s->ns,owner,h,m->data,m->length);
  else if(m->h.opcode==OP_NS_PREAD)n=sf_ns_pread(s->ns,owner,h,m->data,m->length,m->offset);
  else n=sf_ns_enumerate(s->ns,owner,h,(char*)m->data,m->length);
  if(n<0)return n;m->length=(uint32_t)n;return (int64_t)n;
 case OP_NS_ENUMERATE_AT:
  if(m->offset>SF_NS_NODE_MAX)return SF_INVALID;
  n=sf_ns_enumerate_at(s->ns,owner,h,(uint32_t)m->offset,(char*)m->data,m->length);
  if(n<0)return n;m->length=(uint32_t)n;return (int64_t)n;
 case OP_NS_WRITE:
  if(m->offset)return SF_INVALID;return (int64_t)sf_ns_write(s->ns,owner,h,m->data,m->length);
 case OP_NS_STAT:{
  if(m->offset||m->length)return SF_INVALID;SfFileStat stat;
  r=sf_ns_stat(s->ns,owner,h,&stat);if(r)return r;
  memcpy(m->data,&stat,sizeof(stat));m->length=sizeof(stat);return 0;
 }
 case OP_NS_TRUNCATE:
  if(m->length)return SF_INVALID;return sf_ns_truncate(s->ns,owner,h,m->offset);
 case OP_NS_SEEK:
  if(m->length)return SF_INVALID;return (int64_t)sf_ns_seek(s->ns,owner,h,m->offset);
 case OP_NS_POLL:
  if(m->length||m->offset)return SF_INVALID;return (int64_t)sf_ns_poll(s->ns,owner,h);
 case OP_NS_DUP:
  if(m->length||m->offset)return SF_INVALID;r=sf_ns_dup(s->ns,owner,h);
  if(r<0)return r;m->handle=(uint32_t)r;return 0;
 case OP_NS_CLOSE:
  if(m->length||m->offset)return SF_INVALID;return sf_ns_close(s->ns,owner,h);
 }
 return SF_INVALID;
}
int64_t sf_nsr_dispatch(SfNsControl*s,uint32_t caller,SfMessage*m){
 if(!s||!s->ns||!m)return SF_INVALID;
 int64_t r;
 if(caller!=1||m->h.flags!=caller)r=SF_DENIED;
 else if(m->h.size!=40||m->h.protocol_major!=1||m->h.protocol_minor||
   m->h.payload_bytes!=sizeof(*m)-40||m->h.cap_count||!m->h.request_id||m->status||m->length>sizeof(m->data))r=SF_INVALID;
 else r=dispatch(s,m);
 unsigned op=m->h.opcode;
 if(r<0||(op!=OP_NS_READ&&op!=OP_NS_PREAD&&op!=OP_NS_ENUMERATE&&op!=OP_NS_ENUMERATE_AT&&op!=OP_NS_STAT))m->length=0;
 m->status=r;return r;
}
