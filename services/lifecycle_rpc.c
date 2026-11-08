#include "lifecycle_rpc.h"
/* 生命周期 RPC 分发层：只做身份检查、请求解码和状态机调用，不复制
 * manager.c 的状态转换。所有槽位越界、非法消息和不安全操作都在改变
 * 任何状态之前拒绝。回复复用请求消息，status 永远由本层填写。 */
static void reply(SfMessage*m,int64_t status,uint32_t length){m->status=status;m->length=length;if(status)m->length=0;}
int sf_lifecycle_dispatch(SfManager*m,uint32_t caller,uint32_t root,SfMessage*msg){
 if(!m||!msg)return SF_INVALID;
 if(msg->h.size!=40||msg->h.protocol_major!=1||msg->h.protocol_minor||msg->h.payload_bytes>sizeof(*msg)-sizeof(SfIpcHeader))return SF_INVALID;
 if(caller!=root){reply(msg,SF_DENIED,0);return 0;}
 unsigned slot=msg->mode;
 switch(msg->h.opcode){
  case OP_MODULE_STATUS:if(slot>=16||!m->modules[slot].state){reply(msg,SF_NOT_FOUND,0);break;}
   {const SfModule*x=&m->modules[slot];size_t n=strlen(x->id)+1;memcpy(msg->data,x->id,n);msg->mode=x->state;msg->offset=x->generation;reply(msg,0,(uint32_t)n);}break;
  case OP_MODULE_LIST:if(msg->length<16){reply(msg,SF_NO_SPACE,0);break;}
   for(unsigned i=0;i<16;i++)msg->data[i]=(uint8_t)m->modules[i].state;reply(msg,0,16);break;
  case OP_MODULE_START:case OP_MODULE_PREPARE:case OP_MODULE_CANCEL:
  case OP_MODULE_STOP:case OP_MODULE_UNLOAD:{
   if(slot>=16){reply(msg,SF_INVALID,0);break;}
   int r=msg->h.opcode==OP_MODULE_START?sfmod_start(m,slot):
    msg->h.opcode==OP_MODULE_PREPARE?sfmod_prepare(m,slot):
    msg->h.opcode==OP_MODULE_CANCEL?sfmod_cancel(m,slot):
    msg->h.opcode==OP_MODULE_STOP?sfmod_stop(m,slot):sfmod_unload(m,slot);
   reply(msg,r?r:0,0);}break;
  case OP_MODULE_ACQUIRE:case OP_MODULE_RELEASE:{
   unsigned r=msg->offset>UINT32_MAX?RES_COUNT:(unsigned)msg->offset;
   if(slot>=16||r>=RES_COUNT){reply(msg,SF_INVALID,0);break;}
   int rc=msg->h.opcode==OP_MODULE_ACQUIRE?sfmod_acquire(m,slot,r):sfmod_release(m,slot,r);
   reply(msg,rc?rc:0,0);}break;
  default:reply(msg,SF_INVALID,0);break;
 }
 return 0;
}
