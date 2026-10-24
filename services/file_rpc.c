#include "file_rpc.h"
long sf_file_dispatch(SfNamespace*ns,uint32_t owner,uint32_t*root,SfMessage*m){
 if(!ns||!root||!m)return SF_INVALID;
 unsigned op=m->h.opcode;long status=SF_INVALID;
 /* Wire storage is 64-bit, namespace handles are canonical 32-bit values.
  * Reject aliases before conversion, mutation or root-handle bookkeeping. */
 if(m->handle>UINT32_MAX)switch(op){
  case OP_READ:case OP_WRITE:case OP_CLOSE:case OP_SEEK:case OP_POLL:
  case OP_ENUMERATE:case OP_OPENAT:case OP_TRUNCATE:case OP_UNLINK:case OP_STAT:
   m->length=0;m->status=SF_STALE_HANDLE;return SF_STALE_HANDLE;
 }
 if((op==OP_READ||op==OP_WRITE||op==OP_ENUMERATE)&&m->length>sizeof(m->data)){
  m->length=0;m->status=SF_INVALID;return SF_INVALID;
 }
 switch(op){
 case OP_STAT:{
  unsigned length=m->length;m->length=0;if(length)break;SfFileStat info;
  status=sf_ns_stat(ns,owner,m->handle,&info);
  if(!status){memcpy(m->data,&info,sizeof(info));m->length=sizeof(info);}break;
 }
 case OP_ROOT:status=*root?0:SF_DENIED;m->handle=*root;break;
 case OP_OPENAT:
 case OP_UNLINK:
  if(!m->length||m->length>sizeof(m->data)||m->data[m->length-1]||strlen((char*)m->data)+1!=m->length)break;
  if(op==OP_UNLINK){status=sf_ns_unlink(ns,owner,m->handle,(char*)m->data);break;}
  status=sf_ns_openat(ns,owner,m->handle,(char*)m->data,m->mode);if(status>=0){m->handle=status;status=0;}break;
 case OP_OPEN:
  if(!m->length||m->length>sizeof(m->data)||m->data[m->length-1]||strlen((char*)m->data)+1!=m->length)break;
  if(strcmp((char*)m->data,"/dev/console")||m->mode!=OPEN_WRITE){status=SF_DENIED;break;}
  status=sf_ns_open(ns,owner,"/",(char*)m->data,m->mode);if(status>=0){m->handle=status;status=0;}break;
 case OP_WRITE:status=sf_ns_write(ns,owner,m->handle,m->data,m->length);break;
 case OP_TRUNCATE:status=sf_ns_truncate(ns,owner,m->handle,m->offset);break;
 case OP_READ:status=sf_ns_read(ns,owner,m->handle,m->data,m->length);if(status>=0)m->length=status;break;
 case OP_CLOSE:status=sf_ns_close(ns,owner,m->handle);if(!status&&m->handle==*root)*root=0;break;
 case OP_SEEK:status=sf_ns_seek(ns,owner,m->handle,m->offset);break;
 case OP_POLL:status=sf_ns_poll(ns,owner,m->handle);break;
 case OP_ENUMERATE:status=sf_ns_enumerate(ns,owner,m->handle,(char*)m->data,m->length);if(status>=0)m->length=status;break;
 }
 m->status=status;return status;
}
