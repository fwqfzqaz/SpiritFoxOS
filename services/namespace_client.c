#include "namespace_client.h"
static void put32(uint8_t*p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
static void message(SfMessage*m,unsigned op){memset(m,0,sizeof(*m));m->h.size=40;m->h.protocol_major=1;m->h.payload_bytes=sizeof(*m)-40;m->h.opcode=op;}
void sf_nsc_lost(SfNsClient*c){
 if(!c||!c->ready)return;c->ready=0;sf_nsh_invalidate(&c->map);if(c->lost)c->lost(c->context);
}
static int64_t bad(SfNsClient*c){sf_nsc_lost(c);return SF_BAD_FORMAT;}
static int64_t call(SfNsClient*c,SfMessage*m){
 if(!c||!c->ready||!c->exchange||!c->peer)return SF_DEVICE_GONE;
 if(c->sequence==UINT64_MAX)return SF_NO_SPACE;
 uint64_t id=++c->sequence,oldhandle=m->handle,oldoffset=m->offset;
 uint32_t op=m->h.opcode,mode=m->mode,wanted=m->length;
 m->h.request_id=id;int64_t from=c->exchange(c->context,m);
 if(from<0){sf_nsc_lost(c);return from;}
 if((uint64_t)from!=c->peer||m->h.flags!=c->peer||m->h.request_id!=id||m->h.opcode!=op||
    m->h.size!=40||m->h.protocol_major!=1||m->h.protocol_minor||m->h.payload_bytes!=sizeof(*m)-40||
    m->h.cap_count||m->h.deadline_ns||m->length>sizeof(m->data)||m->mode!=mode)return bad(c);
 if(op!=OP_NS_MOUNT_BEGIN&&m->offset!=oldoffset)return bad(c);
 if(op!=OP_NS_OPEN&&op!=OP_NS_OPENAT&&op!=OP_NS_DUP&&m->handle!=oldhandle)return bad(c);
 if(m->status<0){if(m->length||m->status<INT32_MIN)return bad(c);return m->status;}
 switch(op){
 case OP_NS_READ:case OP_NS_PREAD:case OP_NS_ENUMERATE:case OP_NS_ENUMERATE_AT:
  if((uint64_t)m->status>wanted||m->length!=(uint64_t)m->status)return bad(c);break;
 case OP_NS_WRITE:
  if((uint64_t)m->status>wanted||m->length)return bad(c);break;
 case OP_NS_SEEK:
  if(m->length||m->status!=(int64_t)oldoffset||(uint64_t)m->status>UINT32_MAX)return bad(c);break;
 case OP_NS_POLL:
  if(m->length||m->status>7)return bad(c);break;
 case OP_NS_STAT:
  if(m->status||m->length!=sizeof(SfFileStat))return bad(c);break;
 default:
  if(m->status||m->length)return bad(c);break;
 }
 if(op==OP_NS_MOUNT_BEGIN&&!m->offset)return bad(c);
 return m->status;
}
int sf_nsc_connect(SfNsClient*c,uint32_t peer,SfNsExchange exchange,void*context,void(*lost)(void*),uint32_t mask){
 if(!c||!peer||!exchange)return SF_INVALID;
 if(!c->map.epoch)sf_nsh_init(&c->map);else sf_nsc_lost(c);
 c->peer=peer;c->exchange=exchange;c->context=context;c->lost=lost;c->ready=1;
 SfMessage m;message(&m,OP_NS_INIT);m.mode=mask;int64_t r=call(c,&m);
 if(r<0){sf_nsc_lost(c);return(int)r;}return 0;
}
static int path(SfMessage*m,uint32_t owner,uint32_t uid,uint32_t gid,uint32_t value,const char*a,const char*b){
 if(!a)return SF_INVALID;
 size_t an=0;while(an<96&&a[an])an++;if(!an||an==96)return SF_INVALID;
 size_t bn=0;if(b){while(bn<96&&b[bn])bn++;if(!bn||bn==96)return SF_INVALID;}
 put32(m->data,owner);put32(m->data+4,uid);put32(m->data+8,gid);put32(m->data+12,value);
 memcpy(m->data+16,a,an+1);m->length=(uint32_t)(17+an);
 if(b){memcpy(m->data+m->length,b,bn+1);m->length+=(uint32_t)(bn+1);}return 0;
}
static int simple_path(SfNsClient*c,unsigned op,const char*a,uint32_t mode,uint64_t offset,uint32_t uid,uint32_t gid,uint32_t value){
 SfMessage m;message(&m,op);m.mode=mode;m.offset=offset;int r=path(&m,0,uid,gid,value,a,0);return r?r:(int)call(c,&m);
}
int sf_nsc_backends(SfNsClient*c,uint32_t mask){SfMessage m;message(&m,OP_NS_BACKENDS);m.mode=mask;return(int)call(c,&m);}
int sf_nsc_create(SfNsClient*c,const char*a,unsigned type,int readonly,uint32_t uid,uint32_t gid,unsigned perm){
 if(readonly!=0&&readonly!=1)return SF_INVALID;return simple_path(c,OP_NS_CREATE,a,type,(uint32_t)readonly,uid,gid,perm);
}
int sf_nsc_assign(SfNsClient*c,const char*a,uint32_t uid,uint32_t gid,unsigned perm){return simple_path(c,OP_NS_ASSIGN,a,0,0,uid,gid,perm);}
int sf_nsc_project(SfNsClient*c,const char*a,unsigned id){return simple_path(c,OP_NS_PROJECT,a,id,0,0,0,0);}
int sf_nsc_protect(SfNsClient*c,const char*a){return simple_path(c,OP_NS_PROTECT,a,0,0,0,0,0);}
int sf_nsc_mount_check(SfNsClient*c,const char*a){return simple_path(c,OP_NS_MOUNT_CHECK,a,0,0,0,0,0);}
static int new_handle(SfNsClient*c,SfMessage*m,const SfNsHandleReservation*reservation){
 int64_t r=call(c,m);if(r<0)return(int)r;
 int h=sf_nsh_commit(&c->map,reservation,m->handle);if(h<0)return(int)bad(c);return h;
}
int sf_nsc_open(SfNsClient*c,uint32_t owner,const char*root,const char*a,unsigned mode,uint32_t uid,uint32_t gid){
 if(!c)return SF_INVALID;SfNsHandleReservation reservation;int r=sf_nsh_prepare(&c->map,owner,&reservation);if(r)return r;
 SfMessage m;message(&m,OP_NS_OPEN);m.mode=mode;r=path(&m,owner,uid,gid,0,root,a);
 return r?r:new_handle(c,&m,&reservation);
}
int sf_nsc_openat(SfNsClient*c,uint32_t owner,uint64_t parent,const char*a,unsigned mode,uint32_t uid,uint32_t gid){
 if(!c)return SF_INVALID;uint32_t remote;int r=sf_nsh_lookup(&c->map,owner,parent,&remote);if(r)return r;
 SfNsHandleReservation reservation;r=sf_nsh_prepare(&c->map,owner,&reservation);if(r)return r;
 SfMessage m;message(&m,OP_NS_OPENAT);m.handle=remote;m.mode=mode;r=path(&m,owner,uid,gid,0,a,0);
 return r?r:new_handle(c,&m,&reservation);
}
int sf_nsc_remove(SfNsClient*c,uint32_t owner,uint64_t h,const char*a,uint32_t uid,uint32_t gid,int directory){
 if(!c)return SF_INVALID;uint32_t remote;int r=sf_nsh_lookup(&c->map,owner,h,&remote);if(r)return r;
 SfMessage m;message(&m,directory?OP_NS_RMDIR:OP_NS_UNLINK);m.handle=remote;r=path(&m,owner,uid,gid,0,a,0);
 return r?r:(int)call(c,&m);
}
static int handle_message(SfNsClient*c,SfMessage*m,unsigned op,uint32_t owner,uint64_t h){
 if(!c)return SF_INVALID;uint32_t remote;int r=sf_nsh_lookup(&c->map,owner,h,&remote);if(r)return r;
 message(m,op);m->mode=owner;m->handle=remote;return 0;
}
int sf_nsc_dup(SfNsClient*c,uint32_t owner,uint64_t h){
 SfMessage m;int r=handle_message(c,&m,OP_NS_DUP,owner,h);if(r)return r;
 SfNsHandleReservation reservation;r=sf_nsh_prepare(&c->map,owner,&reservation);return r?r:new_handle(c,&m,&reservation);
}
int sf_nsc_close(SfNsClient*c,uint32_t owner,uint64_t h){
 SfMessage m;int r=handle_message(c,&m,OP_NS_CLOSE,owner,h);if(r)return r;r=(int)call(c,&m);
 return r?r:sf_nsh_forget(&c->map,owner,h);
}
int sf_nsc_stat(SfNsClient*c,uint32_t owner,uint64_t h,SfFileStat*out){
 if(!out)return SF_INVALID;SfMessage m;int r=handle_message(c,&m,OP_NS_STAT,owner,h);if(r)return r;
 r=(int)call(c,&m);if(r)return r;SfFileStat info;memcpy(&info,m.data,sizeof(info));
 if(info.bytes!=sizeof(info)||info.type<NODE_DIR||info.type>NODE_PROJECTION||
    (info.mode&~15u)||(info.flags&~7u)||info.offset>UINT32_MAX)return(int)bad(c);
 memcpy(out,&info,sizeof(info));return 0;
}
static int64_t read(SfNsClient*c,unsigned op,uint32_t owner,uint64_t h,void*out,size_t bytes,uint64_t offset){
 if(!out||bytes>sizeof(((SfMessage*)0)->data))return SF_INVALID;
 SfMessage m;int r=handle_message(c,&m,op,owner,h);if(r)return r;m.length=(uint32_t)bytes;m.offset=offset;
 int64_t n=call(c,&m);if(n>=0)memcpy(out,m.data,(size_t)n);return n;
}
int64_t sf_nsc_read(SfNsClient*c,uint32_t owner,uint64_t h,void*out,size_t bytes,uint64_t offset,int positional){
 return read(c,positional?OP_NS_PREAD:OP_NS_READ,owner,h,out,bytes,positional?offset:0);
}
int64_t sf_nsc_enumerate(SfNsClient*c,uint32_t owner,uint64_t h,char*out,size_t bytes){return read(c,OP_NS_ENUMERATE,owner,h,out,bytes,0);}
int64_t sf_nsc_enumerate_at(SfNsClient*c,uint32_t owner,uint64_t h,uint32_t index,char*out,size_t bytes){return read(c,OP_NS_ENUMERATE_AT,owner,h,out,bytes,index);}
int64_t sf_nsc_write(SfNsClient*c,uint32_t owner,uint64_t h,const void*data,size_t bytes){
 if(!data||bytes>sizeof(((SfMessage*)0)->data))return SF_INVALID;
 SfMessage m;int r=handle_message(c,&m,OP_NS_WRITE,owner,h);if(r)return r;
 m.length=(uint32_t)bytes;memcpy(m.data,data,bytes);return call(c,&m);
}
int sf_nsc_truncate(SfNsClient*c,uint32_t owner,uint64_t h,uint64_t bytes){
 SfMessage m;int r=handle_message(c,&m,OP_NS_TRUNCATE,owner,h);if(r)return r;m.offset=bytes;return(int)call(c,&m);
}
int64_t sf_nsc_seek(SfNsClient*c,uint32_t owner,uint64_t h,uint64_t offset){
 SfMessage m;int r=handle_message(c,&m,OP_NS_SEEK,owner,h);if(r)return r;m.offset=offset;return call(c,&m);
}
int64_t sf_nsc_poll(SfNsClient*c,uint32_t owner,uint64_t h){SfMessage m;int r=handle_message(c,&m,OP_NS_POLL,owner,h);return r?r:call(c,&m);}
int sf_nsc_handoff(SfNsClient*c,uint32_t owner,uint64_t h,uint32_t next){
 if(!next)return SF_INVALID;SfMessage m;int r=handle_message(c,&m,OP_NS_HANDOFF,owner,h);if(r)return r;m.offset=next;
 r=(int)call(c,&m);if(r)return r;r=sf_nsh_handoff(&c->map,owner,h,next);return r?(int)bad(c):0;
}
int sf_nsc_reap(SfNsClient*c,uint32_t owner){
 if(!owner)return SF_INVALID;SfMessage m;message(&m,OP_NS_REAP);m.mode=owner;int r=(int)call(c,&m);
 if(!r)sf_nsh_reap(&c->map,owner);return r;
}
int sf_nsc_unmount(SfNsClient*c,unsigned tag){
 if(!tag)return SF_INVALID;SfMessage m;message(&m,OP_NS_UNMOUNT);m.mode=tag;return(int)call(c,&m);
}
int sf_nsc_mount_publish(SfNsClient*c,const SfMountEntry*entries,unsigned count,unsigned tag){
 if(!entries||!count||count>SF_NS_NODE_MAX||!tag)return SF_INVALID;
 for(unsigned i=0;i<count;i++){size_t n=0;while(n<sizeof(entries[i].path)&&entries[i].path[n])n++;if(n==sizeof(entries[i].path))return SF_INVALID;}
 SfMessage m;message(&m,OP_NS_MOUNT_BEGIN);m.mode=count;m.handle=tag;int r=(int)call(c,&m);if(r)return r;
 uint64_t token=m.offset;
 for(unsigned i=0;i<count;i++){
  message(&m,OP_NS_MOUNT_ENTRY);m.offset=token;
  const SfMountEntry*e=&entries[i];uint32_t fields[]={i,e->type,e->readonly,e->external,e->size};
  for(unsigned j=0;j<5;j++)put32(m.data+j*4,fields[j]);
  size_t n=strlen(e->path)+1;memcpy(m.data+20,e->path,n);m.length=(uint32_t)(20+n);
  r=(int)call(c,&m);if(r){if(c->ready){message(&m,OP_NS_MOUNT_ABORT);m.offset=token;call(c,&m);}return r;}
 }
 message(&m,OP_NS_MOUNT_COMMIT);m.offset=token;return(int)call(c,&m);
}
