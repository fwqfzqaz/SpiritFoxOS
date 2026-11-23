#include "persist_driver_info.h"
#include "block_rpc.h"

static int inquiry_error(int64_t result){
 return result>=SF_VERSION_REJECTED&&result<0?(int)result:SF_BAD_FORMAT;
}

static int reject_receipt(SfPersistDriverInquiry*state,const SfPersistDriverTransport*transport,
                          const SfMessage*message){
 if(message->h.cap_count==1&&message->handle){
  int64_t closed=transport->close(transport->context,message->handle);
  if(closed&&closed!=SF_STALE_HANDLE)state->fatal=1;
 }
 return SF_BAD_FORMAT;
}

int persist_driver_info_dispatch(SfPersistDriverInquiry*state,const SfPersistStage*stage,
                                int64_t sender,const SfMessage*request,uint64_t now,int available,
                                const SfPersistDriverTransport*transport,SfPersistDriverInfo*out){
 if(!state||!stage||!request||!transport||!transport->send||!transport->receive||
    !transport->close||!transport->now||!out)return SF_INVALID;
 if(request->h.cap_count)return reject_receipt(state,transport,request);
 if(sender!=1)return SF_DENIED;
 if(request->h.size!=sizeof(SfIpcHeader)||request->h.protocol_major!=1||request->h.protocol_minor||
    request->h.opcode!=OP_PERSIST_CANDIDATE_DRIVER_INFO||request->h.flags!=1||
    request->h.payload_bytes!=sizeof(*request)-sizeof(SfIpcHeader)||!request->h.request_id||
    request->status||request->handle||request->mode||!request->offset||
    request->length!=sizeof(SfPersistStageBinding))return SF_BAD_FORMAT;
 for(size_t i=sizeof(SfPersistStageBinding);i<sizeof(request->data);i++)
  if(request->data[i])return SF_BAD_FORMAT;
 uint64_t deadline=request->h.deadline_ns;
 if(deadline<=now)return SF_TIMEOUT;
 if(deadline-now>SF_PERSIST_STAGE_NS)return SF_BAD_FORMAT;
 if(request->h.request_id<=state->last_nonce)return SF_STALE_HANDLE;
 if(available!=1||stage->phase!=SF_PERSIST_STAGE_READY||state->attempted||state->fatal)return SF_DRAINING;
 if(!stage->send||!stage->reply||stage->send>INT32_MAX||stage->reply>INT32_MAX||
    stage->send==stage->reply||!stage->binding.token||stage->binding.driver_pid<=1||
    stage->binding.driver_pid>INT32_MAX||stage->binding.writable!=1||
    stage->binding.capacity%512||stage->binding.capacity/512<SF_TXN_SECTORS)return SF_BAD_FORMAT;
 if(memcmp(request->data,&stage->binding,sizeof(stage->binding)))return SF_STALE_HANDLE;

 /* The driver echoes the lease serial and sealed token in the normal INFO
  * reply. These tags identify this request even on a shared reply endpoint. */
 state->last_nonce=request->h.request_id;
 state->attempted=1;
 SfMessage message;
 memset(&message,0,sizeof(message));
 message.h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_BLOCK_INFO,0,state->last_nonce,
                        sizeof(message)-sizeof(SfIpcHeader),0,deadline};
 message.offset=request->offset;
 message.handle=stage->binding.token;
 SfBlockRequest expected=sf_block_request(&message);
 int64_t sent=transport->send(transport->context,stage->send,&message);
 if(sent)return inquiry_error(sent);
 int64_t from=transport->receive(transport->context,stage->reply,&message,deadline);
 if(from<0)return inquiry_error(from);
 if(message.h.cap_count)return reject_receipt(state,transport,&message);
 if(from!=(int64_t)stage->binding.driver_pid)return SF_STALE_HANDLE;
 int result=sf_block_reply(&expected,&message,(long)from,stage->binding.driver_pid);
 if(result)return result;
 uint64_t finished;
 result=transport->now(transport->context,&finished);
 if(result)return inquiry_error(result);
 if(finished<now)return SF_BAD_FORMAT;
 if(finished>=deadline)return SF_TIMEOUT;
 for(size_t i=message.length;i<sizeof(message.data);i++)if(message.data[i])return SF_BAD_FORMAT;
 if(message.status)return (int)message.status;
 SfBlockInfo info;
 memcpy(&info,message.data,sizeof(info));
 if(info.sectors!=stage->binding.capacity/512||info.sector_bytes!=512||info.readonly)return SF_BAD_FORMAT;
 *out=(SfPersistDriverInfo){stage->binding,info};
 return 0;
}

void persist_driver_info_reply(SfMessage*message,int status,uint64_t now,
                              const SfPersistDriverInfo*info){
 if(!status&&!info)status=SF_INVALID;
 uint64_t deadline=message->h.deadline_ns;
 if(deadline<=now||deadline-now>SF_PERSIST_STAGE_NS){
  deadline=now>UINT64_MAX-SF_PERSIST_STAGE_NS?UINT64_MAX:now+SF_PERSIST_STAGE_NS;
  if(!status)status=SF_TIMEOUT;
 }
 uint64_t nonce=message->h.request_id;
 message->h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_PERSIST_CANDIDATE_DRIVER_INFO,0,
                         nonce,sizeof(*message)-sizeof(SfIpcHeader),0,deadline};
 message->status=status;
 message->handle=0;
 if(status)message->offset=0;
 message->mode=0;
 message->length=status?0:sizeof(*info);
 memset(message->data,0,sizeof(message->data));
 if(!status)memcpy(message->data,info,sizeof(*info));
}
