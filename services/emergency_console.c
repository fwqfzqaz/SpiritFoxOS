#include "emergency_console.h"
#include "namespace.h"
int64_t sf_emergency_console(SfEmergencyConsole*c,uint32_t*generation,uint32_t owner,SfMessage*m,long(*write)(const void*,size_t)){
 if(!c||!generation||!owner||!m||!write)return SF_INVALID;
 int64_t status=SF_INVALID;unsigned length=m->length;m->length=0;
 if(m->h.size!=40||m->h.protocol_major!=1||m->h.protocol_minor||m->h.flags!=owner||
    m->h.payload_bytes!=sizeof(*m)-40||m->h.cap_count||m->status||length>sizeof(m->data))goto done;
 if(m->h.opcode==OP_ROOT){m->handle=0;status=SF_DENIED;goto done;}
 if(m->h.opcode==OP_OPEN){
  if(m->handle||m->offset||length!=13||memcmp(m->data,"/dev/console",13)||m->mode!=OPEN_WRITE){status=SF_DENIED;goto done;}
  if(c->handle){status=SF_BUSY;goto done;}
  if(*generation>=0x7fffffu){status=SF_NO_SPACE;goto done;}
  c->owner=owner;c->handle=(++*generation<<8)|255u;m->handle=c->handle;status=0;goto done;
 }
 if(m->handle>UINT32_MAX||!c->handle||c->owner!=owner||m->handle!=c->handle){status=SF_STALE_HANDLE;goto done;}
 if(m->mode||(m->offset&&m->h.opcode!=OP_SEEK&&m->h.opcode!=OP_PREAD&&m->h.opcode!=OP_TRUNCATE&&m->h.opcode!=OP_ENUMERATE_AT))goto done;
 switch(m->h.opcode){
 case OP_WRITE:
  status=write(m->data,length);if(status>(int64_t)length)status=SF_BAD_FORMAT;if(status>=0)m->length=length;break;
 case OP_CLOSE:
  if(length)break;c->handle=c->owner=0;status=0;break;
 case OP_STAT:{
  if(length)break;SfFileStat info={32,NODE_CONSOLE,OPEN_WRITE,0,0,0};memcpy(m->data,&info,sizeof(info));m->length=sizeof(info);status=0;break;
 }
 case OP_POLL:if(!length)status=2;break;
 case OP_READ:case OP_PREAD:case OP_ENUMERATE:case OP_ENUMERATE_AT:status=SF_DENIED;break;
 case OP_SEEK:status=SF_NOT_SEEKABLE;break;
 case OP_DUP:if(!length)status=SF_NO_SPACE;break;
 }
done:m->status=status;return status;
}
