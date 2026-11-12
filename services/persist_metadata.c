#include "persist_server.h"
/* This narrow request is independent of attach/mount/update operations. The
 * sender comes from RECV, never a request payload. Kernel flags must agree. */
int persist_metadata_dispatch(int64_t sender,SfMessage*m){
 if(sender!=1)return SF_DENIED;
 if(!m||m->h.size!=sizeof(SfIpcHeader)||m->h.protocol_major!=1||
    m->h.protocol_minor||m->h.opcode!=OP_PERSIST_NOTES_SIZE||
    m->h.flags!=1||!m->h.request_id||m->h.cap_count||
    m->h.payload_bytes!=sizeof(*m)-sizeof(SfIpcHeader)||
    m->status||m->handle||m->offset||m->length||m->mode)return SF_INVALID;
 uint64_t bytes=0;int r=persist_notes_size(&bytes);if(r)return r;
 m->handle=bytes;return 0;
}
