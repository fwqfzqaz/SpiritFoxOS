#include "namespace_transport.h"
static int64_t wait_reply(SfNsTransport*t,SfMessage*m,uint64_t id,unsigned op){
 uint64_t start=t->ops.ticks(t->context);
 if(start>UINT64_MAX-3000)return SF_INVALID;
 uint64_t end=start+3000,previous=start;
 for(unsigned waits=0;waits<6000;waits++){
  uint64_t now=t->ops.ticks(t->context);if(now<previous)return SF_BAD_FORMAT;previous=now;
  if(now>=end)return SF_TIMEOUT;
  int64_t from=t->ops.receive(t->context,t->reply,m,1);
  if(from>=0){
   if(m->h.cap_count==1)t->ops.close(t->context,m->handle);
   if(from!=t->peer||m->h.request_id!=id)return SF_STALE_HANDLE;
   if(m->h.flags!=t->peer||m->h.size!=40||m->h.protocol_major!=1||m->h.protocol_minor||
      m->h.opcode!=op||m->h.payload_bytes!=sizeof(*m)-40||m->h.cap_count||m->h.deadline_ns)return SF_BAD_FORMAT;
   return from;
  }
  if(from!=SF_TIMEOUT)return from;
  if(!t->ops.alive(t->context,t->peer))return SF_DEVICE_GONE;
  int r=t->ops.pump(t->context,1);if(r<0)return r;
 }
 return SF_TIMEOUT;
}
int64_t sf_nst_exchange(SfNsTransport*t,SfMessage*m){
 if(!t||!m||!t->service||!t->reply||!t->peer||!m->h.request_id||
    !t->ops.send||!t->ops.receive||!t->ops.ticks||!t->ops.alive||!t->ops.pump||!t->ops.close)return SF_INVALID;
 if(t->inflight)return SF_BUSY;
 if(!t->ops.alive(t->context,t->peer))return SF_DEVICE_GONE;
 t->inflight=1;uint64_t id=m->h.request_id;unsigned op=m->h.opcode;
 int64_t r=t->ops.send(t->context,t->service,m);
 if(!r)r=wait_reply(t,m,id,op);else if(r>0)r=SF_BAD_FORMAT;
 t->inflight=0;return r;
}
