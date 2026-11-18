#include "libsf.h"
#include "persist_server.h"
#include "persist_terminal.h"
#include "persist_idle.h"

static uint64_t stop_limit(uint64_t now){
 return now>UINT64_MAX-SF_PERSIST_STOP_NS?UINT64_MAX:now+SF_PERSIST_STOP_NS;
}
int persist_terminal_dispatch(SfPersistTerminal*s,int64_t sender,SfMessage*m,uint64_t now){
 if(!s||!m)return SF_INVALID;
 if(s->accepted)return SF_DRAINING;
 if(sender!=1)return SF_DENIED;
 if(m->h.size!=sizeof(SfIpcHeader)||m->h.protocol_major!=1||m->h.protocol_minor||
    m->h.opcode!=OP_PERSIST_STOP||m->h.flags!=1||!m->h.request_id||m->h.cap_count||
    m->h.payload_bytes!=sizeof(*m)-sizeof(SfIpcHeader)||
    !m->h.deadline_ns||m->status||m->handle||m->offset||m->length||m->mode)return SF_INVALID;
 for(unsigned i=0;i<sizeof(m->data);i++)if(m->data[i])return SF_INVALID;
 if(m->h.deadline_ns<=now)return SF_TIMEOUT;
 if(m->h.deadline_ns>stop_limit(now))return SF_INVALID;
 if(m->h.request_id<=s->last_nonce)return SF_STALE_HANDLE;
 s->last_nonce=m->h.request_id;
 /* This cached check and the terminal decision execute in one serial
  * dispatch. Never call MOUNT, finish a trial, or issue block I/O here. */
 int r=persist_quiescent();
 if(r)return r<SF_VERSION_REJECTED||r>0?SF_BAD_FORMAT:r;
 s->accepted=1;return 0;
}
static void stop_reply(SfMessage*m,int status,uint64_t now,int accepted){
 uint64_t nonce=m->h.request_id,deadline=m->h.deadline_ns,limit=stop_limit(now);
 /* A valid completion echoes its deadline. Malformed/expired requests must
  * not turn the error response into an unbounded SEND (deadline zero). */
 if(deadline<=now||deadline>limit)deadline=limit;
 m->h=(SfIpcHeader){sizeof(SfIpcHeader),1,0,OP_PERSIST_STOP,0,nonce,
                   sizeof(*m)-sizeof(SfIpcHeader),0,deadline};
 m->status=status;m->handle=m->offset=0;m->length=0;m->mode=accepted?1u:0u;
 for(unsigned i=0;i<sizeof(m->data);i++)m->data[i]=0;
}
typedef struct {const SfBootArgs*args;int64_t(*send)(uint32_t,SfMessage*);} VerifyChannel;
static int64_t verify_channel_send(void*context,SfMessage*m){VerifyChannel*c=context;return c->send(c->args->manager,m);}
static int64_t verify_channel_receive(void*context,SfMessage*m,uint64_t deadline){VerifyChannel*c=context;return sf_recv(c->args->service,m,deadline);}
static int verify_channel_now(void*context,uint64_t*out){
 (void)context;uint64_t ticks=sf_ticks();if(ticks>UINT64_MAX/10000000ull)return SF_BAD_FORMAT;
 *out=ticks*10000000ull;return 0;
}
static void verify_channel_close(void*context,uint64_t cap){(void)context;sf_cap_close(cap);}
static int64_t stage_derive(void*context,uint64_t cap,unsigned rights){(void)context;return sf_cap_derive(cap,rights);}
static int64_t stage_close(void*context,uint64_t cap){(void)context;return sf_cap_close(cap);}
int64_t persist_serve_with_idle(const SfBootArgs*a,int(*ordinary)(SfMessage*),
                              int64_t(*reply_send)(uint32_t,SfMessage*),int(*unbound)(void),SfPersistSnapshotGet snapshot_get,SfPersistVolumeVerify volume_verify){
 if(!a||!a->service||!a->manager||!ordinary||!reply_send)return SF_INVALID;
 SfPersistTerminal terminal={0};
 SfPersistIdle idle={0};SfPersistVerify verification={0};SfPersistStage stage={0};
 SfPersistSnapshot verified_snapshot={0};int verified=0,ordinary_seen=0;
 for(;;){
  SfMessage m;int64_t from=sf_recv(a->service,&m,0);if(from<0)return from;
  if(m.h.opcode==OP_PERSIST_CANDIDATE_PROBE||m.h.opcode==OP_PERSIST_CANDIDATE_DISCARD||
     m.h.opcode==OP_PERSIST_CANDIDATE_CAPS||idle.entered){
   if(m.h.opcode!=OP_PERSIST_CANDIDATE_CAPS&&m.h.cap_count==1)sf_cap_close(m.handle);
   uint64_t ticks=sf_ticks();if(ticks>UINT64_MAX/10000000ull)return SF_TIMEOUT;
   uint64_t now=ticks*10000000ull;
   int control=m.h.opcode==OP_PERSIST_CANDIDATE_PROBE||m.h.opcode==OP_PERSIST_CANDIDATE_DISCARD;
   /* Never evaluate a candidate permission by mounting or attaching. A
    * fresh provider may be privately inspected; ordinary authority stays off. */
   int available=!ordinary_seen&&unbound&&unbound()==1;
   if(m.h.opcode==OP_PERSIST_CANDIDATE_CAPS){
    SfPersistStageCaps caps={0,stage_derive,stage_close};
    if(stage.last_nonce<idle.last_nonce)stage.last_nonce=idle.last_nonce;
    unsigned mode=m.mode;SfPersistStageBinding binding;memset(&binding,0,sizeof(binding));
    int r=persist_stage_dispatch(&stage,from,&m,now,available&&idle.entered&&verified,a->pid,
                                 verified?&verified_snapshot:0,&caps);
    if(idle.last_nonce<stage.last_nonce)idle.last_nonce=stage.last_nonce;
    if(!r)binding=stage.binding;
    persist_stage_reply(&m,r,now,mode,r?0:&binding);reply_send(a->manager,&m);
    if(stage.fatal)return SF_BAD_FORMAT;
    continue;
   }
   if(m.h.opcode==OP_PERSIST_CANDIDATE_VERIFY&&idle.entered){
    VerifyChannel channel={a,reply_send};
    SfPersistVerifyTransport transport={&channel,verify_channel_send,verify_channel_receive,verify_channel_now,verify_channel_close};
    if(verification.last_nonce<idle.last_nonce)verification.last_nonce=idle.last_nonce;
    SfPersistSnapshot snapshot;
    int r=persist_verify_dispatch(&verification,from,&m,now,available,volume_verify,&transport,&snapshot);
    if(!r){verified_snapshot=snapshot;verified=1;}
    if(idle.last_nonce<verification.last_nonce)idle.last_nonce=verification.last_nonce;
    persist_verify_reply(&m,r,now,r?0:&snapshot);reply_send(a->manager,&m);continue;
   }
   int r=control?persist_idle_dispatch(&idle,from,&m,now,available):SF_DRAINING;
   persist_idle_reply(&m,r,now,!r);reply_send(a->manager,&m);
   if(idle.discarded)return 0;
   continue;
  }
  if(from==1)ordinary_seen=1;
  if(m.h.opcode==OP_PERSIST_SNAPSHOT){
   if(m.h.cap_count==1)sf_cap_close(m.handle);
   uint64_t ticks=sf_ticks();if(ticks>UINT64_MAX/10000000ull)return SF_TIMEOUT;
   uint64_t now=ticks*10000000ull;SfPersistSnapshot snapshot;
   int r=persist_snapshot_dispatch(from,&m,now,snapshot_get,&snapshot);
   persist_snapshot_reply(&m,r,now,r?0:&snapshot);reply_send(a->manager,&m);continue;
  }
  if(m.h.opcode==OP_PERSIST_STOP){
   /* RECV installed this handle before parsing or sender authentication. */
   if(m.h.cap_count==1)sf_cap_close(m.handle);
   uint64_t ticks=sf_ticks();if(ticks>UINT64_MAX/10000000ull)return SF_TIMEOUT;
   uint64_t now=ticks*10000000ull;
   int r=persist_terminal_dispatch(&terminal,from,&m,now);stop_reply(&m,r,now,terminal.accepted);
   reply_send(a->manager,&m);
   if(terminal.accepted)return 0;
   continue;
  }
  if(m.h.opcode==OP_PERSIST_NOTES_SIZE||m.h.opcode==OP_PERSIST_QUIESCENT){
   if(m.h.cap_count==1)sf_cap_close(m.handle);
   m.status=persist_metadata_dispatch(from,&m);
  }else m.status=from==1?ordinary(&m):SF_DENIED;
  m.h.cap_count=0;reply_send(a->manager,&m);
 }
}
int64_t persist_serve(const SfBootArgs*a,int(*ordinary)(SfMessage*),
                     int64_t(*reply_send)(uint32_t,SfMessage*)){
 return persist_serve_with_idle(a,ordinary,reply_send,0,0,0);
}
