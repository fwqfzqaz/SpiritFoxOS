#ifndef SF_BLOCK_RPC_H
#define SF_BLOCK_RPC_H
#include "virtio.h"

/* Private client-side response contract, shared by Root and Persist. Keep
 * request metadata rather than another full message on the guest stack. */
typedef struct {
 uint64_t id,deadline,offset,handle;
 unsigned opcode,length,mode;
} SfBlockRequest;
static inline SfBlockRequest sf_block_request(const SfMessage*m){
 return(SfBlockRequest){m->h.request_id,m->h.deadline_ns,m->offset,m->handle,
                       m->h.opcode,m->length,m->mode};
}
/* Return validation failure separately from an authenticated service error.
 * The caller closes any installed reply capability before inspecting fields. */
static inline int sf_block_reply(const SfBlockRequest*q,const SfMessage*m,long from,uint32_t pid){
 if(from!=(long)pid||m->h.request_id!=q->id)return SF_STALE_HANDLE;
 if(m->h.size!=40||m->h.protocol_major!=1||m->h.protocol_minor||
    m->h.flags!=pid||m->h.opcode!=q->opcode||m->h.cap_count||
    m->h.payload_bytes!=sizeof(*m)-40||m->h.deadline_ns!=q->deadline||
    m->status<SF_VERSION_REJECTED||m->status>0||
    m->offset!=q->offset||m->handle!=q->handle||m->mode!=q->mode)return SF_BAD_FORMAT;
 if(m->status)return m->length?SF_BAD_FORMAT:0;
 unsigned expected;
 switch(q->opcode){
 case OP_BLOCK_INFO:if(q->length)return SF_BAD_FORMAT;expected=sizeof(SfBlockInfo);break;
 case OP_BLOCK_READ:case OP_BLOCK_WRITE:if(q->length!=256)return SF_BAD_FORMAT;expected=256;break;
 case OP_BLOCK_FLUSH:case OP_BLOCK_WRITE_RANGE:case OP_BLOCK_STOP:case OP_BLOCK_DMA_PROBE:if(q->length)return SF_BAD_FORMAT;expected=0;break;
 default:return SF_BAD_FORMAT;
 }
 return m->length==expected?0:SF_BAD_FORMAT;
}
#endif
