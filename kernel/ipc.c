#include "kernel.h"
#define ENDPOINTS 64
typedef struct {uint32_t owner,epoch,head,count;SfMessage queue[8];} Endpoint;
static Endpoint endpoints[ENDPOINTS];
static Cap *cap(Task*t,uint64_t h,uint32_t rights){if(h>UINT32_MAX)return 0;unsigned i=(h&255);if(!i||i>32)return 0;Cap*c=&t->caps[i-1];if(!c->endpoint||c->generation!=(h>>8)||(c->rights&rights)!=rights)return 0;Endpoint*e=&endpoints[c->endpoint-1];return e->owner&&c->epoch==e->epoch?c:0;}
static int install(Task*t,unsigned ep,unsigned rights){for(unsigned i=0;i<32;i++)if(!t->caps[i].endpoint){Cap*c=&t->caps[i];if(c->generation>=0x7fffff)continue;c->generation++;c->endpoint=ep+1;c->epoch=endpoints[ep].epoch;c->rights=rights;return(int)((c->generation<<8)|(i+1));}return SF_NO_SPACE;}
int ipc_grant(Task*from,Task*to,uint32_t handle,uint32_t rights){Cap*c=cap(from,handle,RIGHT_DELEGATE);if(!c||(rights&c->rights)!=rights)return SF_DENIED;return install(to,c->endpoint-1,rights);}
static void invalidate(unsigned i){Endpoint*e=&endpoints[i];e->owner=0;e->count=0;e->epoch++;for(unsigned j=0;j<SF_MAX_TASKS;j++){Task*t=&tasks[j];if(t->state==T_BLOCKED&&t->wait_endpoint==i+1){t->frame->rax=SF_REVOKED;t->state=T_READY;t->ready_since=ticks;}for(unsigned k=0;k<32;k++)if(t->caps[k].endpoint==i+1)t->caps[k].endpoint=0;}}
static void close_cap(Cap*c){
 unsigned ep=c->endpoint-1;c->endpoint=0;
 /* Closing a handle drops one reference; other tasks and derived handles
    keep the endpoint alive until its final reference is released. */
 for(unsigned j=0;j<SF_MAX_TASKS;j++)for(unsigned k=0;k<32;k++)if(tasks[j].caps[k].endpoint==ep+1)return;
 invalidate(ep);
}
void ipc_cleanup(Task*t){for(unsigned i=0;i<ENDPOINTS;i++)if(endpoints[i].owner==t->pid)invalidate(i);for(unsigned i=0;i<32;i++)if(t->caps[i].endpoint)close_cap(&t->caps[i]);}
void ipc_deadlines(void){for(unsigned i=0;i<SF_MAX_TASKS;i++){Task*t=&tasks[i];if(t->state==T_BLOCKED&&t->wait_deadline&&ticks*10000000ull>=t->wait_deadline){t->frame->rax=SF_TIMEOUT;t->state=T_READY;t->ready_since=ticks;}}}
long ipc_syscall(uint64_t call,uint64_t a,uint64_t b,uint64_t c){
 Task*t=&tasks[current];
 if(call==SYS_EP_CREATE){for(unsigned i=0;i<ENDPOINTS;i++)if(!endpoints[i].owner){Endpoint*e=&endpoints[i];e->owner=t->pid;e->epoch++;e->head=e->count=0;int h=install(t,i,7);if(h<0)e->owner=0;return h;}return SF_NO_SPACE;}
 if(call==SYS_CAP_CLOSE){Cap*x=cap(t,a,0);if(!x)return SF_STALE_HANDLE;close_cap(x);return 0;}
 if(call==SYS_CAP_DERIVE){Cap*x=cap(t,a,RIGHT_DELEGATE);if(!x||(b&~7)||(b&x->rights)!=b)return SF_DENIED;return install(t,x->endpoint-1,b);}
 if(call==SYS_CAP_REVOKE){Cap*x=cap(t,a,RIGHT_DELEGATE);if(!x||endpoints[x->endpoint-1].owner!=t->pid)return SF_DENIED;invalidate(x->endpoint-1);return 0;}
 if(call==SYS_SEND){
  Cap*x=cap(t,a,RIGHT_SEND);if(!x)return SF_STALE_HANDLE;SfMessage m;if(copy_in(t,&m,b,sizeof(m)))return SF_DENIED;
  if(m.h.size!=40||m.h.protocol_major!=1||m.h.protocol_minor||m.h.payload_bytes>sizeof(m)-40||m.h.cap_count||m.length>SF_MSG_BYTES)return SF_INVALID;
  if(m.h.deadline_ns&&ticks*10000000ull>=m.h.deadline_ns)return SF_TIMEOUT;
  Endpoint*e=&endpoints[x->endpoint-1];m.h.flags=t->pid;
  for(unsigned j=0;j<SF_MAX_TASKS;j++){Task*r=&tasks[j];if(r->state==T_BLOCKED&&r->wait_endpoint==x->endpoint){if(copy_out(r,r->recv_buffer,&m,sizeof(m)))panic("receive mapping changed");r->frame->rax=t->pid;r->state=T_READY;r->ready_since=ticks;return 0;}}
  if(e->count==8)return SF_WOULD_BLOCK;e->queue[(e->head+e->count)%8]=m;e->count++;return 0;
 }
 if(call==SYS_RECV){
  Cap*x=cap(t,a,RIGHT_RECV);if(!x)return SF_STALE_HANDLE;if(!user_range(t,b,sizeof(SfMessage),1))return SF_DENIED;Endpoint*e=&endpoints[x->endpoint-1];
  if(e->count){SfMessage*m=&e->queue[e->head];long sender=m->h.flags;copy_out(t,b,m,sizeof(*m));e->head=(e->head+1)%8;e->count--;return sender;}
  if(c&&ticks*10000000ull>=c)return SF_TIMEOUT;
  t->state=T_BLOCKED;t->wait_endpoint=x->endpoint;t->recv_buffer=b;t->wait_deadline=c;return 0;
 }
 return SF_INVALID;
}
