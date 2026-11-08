#include "kernel.h"
/* IPC：64 个端点、每端点 8 条消息的环形队列、句柄（epoch<<8|槽位）授权模型。
 * 句柄 epoch 使撤销后的旧句柄自然失效；发送方 PID 记录在消息 flags 字段。
 * 每条能力保存派生树锚点：显式 CAP_REVOKE 只撤销自己持有的那一支，
 * 端点所有者撤销仍回收整个端点。cap_count=1 的 SEND 附带一条能力，
 * 接收方在 RECV 时安装同权新句柄；发送方需要 RIGHT_DELEGATE。 */
#define ENDPOINTS 64
#define QUEUE 8
typedef struct {
 uint32_t owner,epoch,head,count;
 SfMessage queue[QUEUE];
 /* 随消息转移的能力在 RECV 时重新校验端点与 epoch；cap_ep 为 0 表示
  * 该队列项没有附带能力。父锚点用于后续派生撤销。 */
 uint32_t cap_ep[QUEUE],cap_epoch[QUEUE],cap_rights[QUEUE];
 uint32_t cap_parent_pid[QUEUE],cap_parent_slot[QUEUE],cap_parent_generation[QUEUE];
} Endpoint;
static Endpoint endpoints[ENDPOINTS];
static uint64_t send_bytes,receive_bytes;
/* 累计 IPC 字节数（饱和至 UINT64_MAX，不回绕）。 */
static void copied(uint64_t*count,size_t bytes){*count=bytes>UINT64_MAX-*count?UINT64_MAX:*count+bytes;}
/* IPC 统计快照（根任务专用系统调用的数据出口）。 */
void ipc_stats_snapshot(SfIpcStats*out){*out=(SfIpcStats){sizeof(*out),SF_IPC_STATS_VERSION,send_bytes,receive_bytes};}
/* 句柄校验：槽位合法、代数匹配、权限位覆盖、端点存活且 epoch 未过期。 */
static Cap *cap(Task*t,uint64_t h,uint32_t rights){if(h>UINT32_MAX)return 0;unsigned i=(h&255);if(!i||i>32)return 0;Cap*c=&task_main(t)->caps[i-1];if(!c->endpoint||c->generation!=(h>>8)||(c->rights&rights)!=rights)return 0;Endpoint*e=&endpoints[c->endpoint-1];return e->owner&&c->epoch==e->epoch?c:0;}
/* 选取空闲能力槽：优先复用 generation 较小的槽（低于回收阈值），
 * 超过任务能力上限即拒绝。 */
static int cap_slot(const Task*t){
 const Task*m=task_main_c(t);
 if(m->cap_limit>SF_CAP_LIMIT_MAX||task_caps_used(t)>=m->cap_limit)return -1;
 for(unsigned i=0;i<SF_CAP_LIMIT_MAX;i++)if(!m->caps[i].endpoint&&m->caps[i].generation<0x7fffff)return (int)i;
 return -1;
}
/* 安装新能力：generation 递增形成新句柄，继承端点当前 epoch。
 * 根能力提交 parent_pid=0；派生与转移提交父能力的 PID、槽位和代数。 */
static int install(Task*t,unsigned ep,unsigned rights,uint32_t parent_pid,unsigned parent_slot,uint32_t parent_generation){
 int slot=cap_slot(t);if(slot<0)return SF_NO_SPACE;Cap*c=&task_main(t)->caps[slot];c->generation++;c->endpoint=ep+1;c->epoch=endpoints[ep].epoch;c->rights=rights;
 c->parent_pid=parent_pid;c->parent_slot=parent_slot;c->parent_generation=parent_generation;
 return(int)((c->generation<<8)|(slot+1));
}
/* 委派能力：要求发送方持有 RIGHT_DELEGATE 且新权限是原权限子集。 */
int ipc_grant(Task*from,Task*to,uint32_t handle,uint32_t rights){Cap*c=cap(from,handle,RIGHT_DELEGATE);if(!c||(rights&c->rights)!=rights)return SF_DENIED;return install(to,c->endpoint-1,rights,from->pid,(unsigned)(handle&255)-1,c->generation);}
/* 端点失效：epoch 递增吊销全部旧句柄，唤醒阻塞在端点上的任务（返回
 * SF_REVOKED），并清空所有任务对该端点的能力引用。 */
static void invalidate(unsigned i){Endpoint*e=&endpoints[i];e->owner=0;e->count=0;e->epoch++;for(unsigned j=0;j<SF_MAX_THREADS;j++){Task*t=&tasks[j];if(t->state==T_BLOCKED&&t->wait_endpoint==i+1){t->frame->rax=SF_REVOKED;task_wake(t);}Cap*caps=task_main(t)->caps;for(unsigned k=0;k<32;k++)if(caps[k].endpoint==i+1)caps[k].endpoint=0;}}
static void close_cap(Cap*c){
 unsigned ep=c->endpoint-1;c->endpoint=0;
 /* Closing a handle drops one reference; other tasks and derived handles
    keep the endpoint alive until its final reference is released. */
 for(unsigned j=0;j<SF_MAX_THREADS;j++){const Cap*caps=task_main_c(&tasks[j])->caps;for(unsigned k=0;k<32;k++)if(caps[k].endpoint==ep+1)return;}
 invalidate(ep);
}
/* 撤销后仍被其他能力引用的端点保持存活；该任务若正阻塞接收且已失去
 * 唯一的接收权，则按端点撤销语义唤醒。 */
static int holds_right(const Task*t,unsigned ep,unsigned right){const Cap*caps=task_main_c(t)->caps;for(unsigned i=0;i<32;i++)if(caps[i].endpoint==ep+1&&(caps[i].rights&right))return 1;return 0;}
static void wake_if_lost(Task*t,unsigned ep){if(t->state!=T_BLOCKED||t->wait_endpoint!=ep+1)return;if(holds_right(t,ep,RIGHT_RECV))return;t->frame->rax=SF_REVOKED;task_wake(t);}
static int has_refs(unsigned ep){for(unsigned j=0;j<SF_MAX_THREADS;j++){const Cap*caps=task_main_c(&tasks[j])->caps;for(unsigned k=0;k<32;k++)if(caps[k].endpoint==ep+1)return 1;}return 0;}
/* 递归撤销 parent 槽的全部后代能力。父锚点同时比对 PID、槽位和代数，
 * 因此槽位复用不会把新的派生关系错误地并入旧树。 */
static void revoke_descendants(uint32_t pid,unsigned slot,uint32_t generation){
 for(unsigned i=0;i<SF_MAX_THREADS;i++){Task*t=&tasks[i];if(!t->pid||!t->main)continue;
  for(unsigned s=0;s<32;s++){Cap*c=&t->caps[s];
   if(!c->endpoint||c->parent_pid!=pid||c->parent_slot!=slot||c->parent_generation!=generation)continue;
   unsigned ep=c->endpoint-1;uint32_t child_generation=c->generation;
   c->endpoint=0;wake_if_lost(t,ep);
   revoke_descendants(t->pid,s,child_generation);
   if(!has_refs(ep))invalidate(ep);
  }
 }
}
/* 撤销一个槽位及其后代；父能力本身先清空，再处理派生树。 */
static void revoke_cap(Task*t,unsigned slot){
 Task*m=task_main(t);Cap*c=&m->caps[slot];if(!c->endpoint)return;
 unsigned ep=c->endpoint-1;uint32_t generation=c->generation;
 c->endpoint=0;wake_if_lost(t,ep);
 revoke_descendants(m->pid,slot,generation);
 if(!has_refs(ep))invalidate(ep);
}
void ipc_cleanup(Task*t){
 Task*m=task_main(t);
 for(unsigned i=0;i<ENDPOINTS;i++)if(endpoints[i].owner==t->pid)invalidate(i);
 for(unsigned i=0;i<32;i++)if(m->caps[i].endpoint)close_cap(&m->caps[i]);
}
/* IPC 截止时间：时钟滴答推进后唤醒已过期的阻塞任务（返回 SF_TIMEOUT）。 */
void ipc_deadlines(void){for(unsigned i=0;i<SF_MAX_THREADS;i++){Task*t=&tasks[i];if(t->state==T_BLOCKED&&t->wait_deadline&&ticks*10000000ull>=t->wait_deadline){t->frame->rax=SF_TIMEOUT;t->irq_waiting=0;task_wake(t);}}}
/* IPC 系统调用：EP_CREATE/CAP_CLOSE/CAP_DERIVE/CAP_REVOKE/SEND_CANCEL/SEND/RECV。
 * 消息头校验（尺寸、协议版本 1.0、载荷上界、至多一条随附能力）在内核完成；
 * 队列满返回 SF_WOULD_BLOCK，deadline 过期返回 SF_TIMEOUT。 */
long ipc_syscall(uint64_t call,uint64_t a,uint64_t b,uint64_t c){
 Task*t=&tasks[current];
 if(call==SYS_EP_CREATE){if(cap_slot(t)<0)return SF_NO_SPACE;for(unsigned i=0;i<ENDPOINTS;i++)if(!endpoints[i].owner){Endpoint*e=&endpoints[i];e->owner=t->pid;e->epoch++;e->head=e->count=0;int h=install(t,i,7,0,0,0);if(h<0)e->owner=0;return h;}return SF_NO_SPACE;}
 if(call==SYS_CAP_CLOSE){Cap*x=cap(t,a,0);if(!x)return SF_STALE_HANDLE;close_cap(x);return 0;}
 if(call==SYS_CAP_DERIVE){Cap*x=cap(t,a,RIGHT_DELEGATE);if(!x||(b&~7)||(b&x->rights)!=b)return SF_DENIED;return install(t,x->endpoint-1,b,t->pid,(unsigned)(a&255)-1,x->generation);}
 if(call==SYS_CAP_REVOKE){
  Cap*x=cap(t,a,RIGHT_DELEGATE);if(!x)return SF_DENIED;
  unsigned ep=x->endpoint-1;
  /* 端点所有者撤销整条端点；其他持有者只撤销自己这一支派生能力。 */
  if(endpoints[ep].owner==t->pid){invalidate(ep);return 0;}
  revoke_cap(t,(unsigned)(a&255)-1);return 0;
 }
 if(call==SYS_SEND_CANCEL){
  Cap*x=cap(t,a,RIGHT_SEND);if(!x)return SF_STALE_HANDLE;if(c)return SF_INVALID;
  Endpoint*e=&endpoints[x->endpoint-1];if(e->head>=8||e->count>8)return SF_BAD_FORMAT;
  unsigned count=e->count,kept=0;
  /* Logical indices cover at most one ring lap. Each destination precedes
   * its source, so compaction cannot overwrite a later unread message. */
  for(unsigned i=0;i<count;i++){
   SfMessage*m=&e->queue[(e->head+i)%8];if(m->h.flags==t->pid&&m->h.request_id==b)continue;
   if(kept!=i){unsigned d=(e->head+kept)%8;e->queue[d]=*m;e->cap_ep[d]=e->cap_ep[(e->head+i)%8];e->cap_epoch[d]=e->cap_epoch[(e->head+i)%8];e->cap_rights[d]=e->cap_rights[(e->head+i)%8];e->cap_parent_pid[d]=e->cap_parent_pid[(e->head+i)%8];e->cap_parent_slot[d]=e->cap_parent_slot[(e->head+i)%8];e->cap_parent_generation[d]=e->cap_parent_generation[(e->head+i)%8];}
   kept++;
  }
  if(kept==count)return 0;
  for(unsigned i=kept;i<count;i++){unsigned d=(e->head+i)%8;memset(&e->queue[d],0,sizeof(SfMessage));e->cap_ep[d]=0;}
  e->count=kept;if(!kept)e->head=0;return count-kept;
 }
 if(call==SYS_SEND){
  Cap*x=cap(t,a,RIGHT_SEND);if(!x)return SF_STALE_HANDLE;SfMessage m;if(copy_in(t,&m,b,sizeof(m)))return SF_DENIED;
  copied(&send_bytes,sizeof(m));
  if(m.h.size!=40||m.h.protocol_major!=1||m.h.protocol_minor||m.h.payload_bytes>sizeof(m)-40||m.h.cap_count>1||m.length>SF_MSG_BYTES)return SF_INVALID;
  if(m.h.deadline_ns&&ticks*10000000ull>=m.h.deadline_ns)return SF_TIMEOUT;
  /* 附带能力：发送方必须持有该能力的 RIGHT_DELEGATE。传输不消耗原能力，
   * 接收方得到同权的新句柄，并作为发送方能力的子节点参与派生撤销。 */
  unsigned attach_ep=0,attach_rights=0,attach_slot=0;uint32_t attach_pid=0,attach_generation=0;
  if(m.h.cap_count){
   Cap*g=cap(t,m.handle,RIGHT_DELEGATE);if(!g)return SF_DENIED;
   attach_ep=g->endpoint-1;attach_rights=g->rights;attach_slot=(unsigned)(m.handle&255)-1;
   attach_pid=t->pid;attach_generation=g->generation;
  }
  Endpoint*e=&endpoints[x->endpoint-1];m.h.flags=t->pid;
  for(unsigned j=0;j<SF_MAX_THREADS;j++){Task*r=&tasks[j];if(r->state==T_BLOCKED&&r->wait_endpoint==x->endpoint){
   if(m.h.cap_count){int h=install(r,attach_ep,attach_rights,attach_pid,attach_slot,attach_generation);if(h<0)return SF_NO_SPACE;m.handle=(uint64_t)(unsigned)h;}
   if(copy_out(r,r->recv_buffer,&m,sizeof(m)))panic("receive mapping changed");copied(&receive_bytes,sizeof(m));r->frame->rax=t->pid;task_wake(r);return 0;}}
  if(e->count==8)return SF_WOULD_BLOCK;unsigned idx=(e->head+e->count)%8;e->queue[idx]=m;
  e->cap_ep[idx]=m.h.cap_count?(attach_ep+1):0;e->cap_epoch[idx]=m.h.cap_count?endpoints[attach_ep].epoch:0;
  e->cap_rights[idx]=attach_rights;e->cap_parent_pid[idx]=attach_pid;e->cap_parent_slot[idx]=attach_slot;e->cap_parent_generation[idx]=attach_generation;
  e->count++;return 0;
 }
 if(call==SYS_RECV){
  Cap*x=cap(t,a,RIGHT_RECV);if(!x)return SF_STALE_HANDLE;if(!user_range(t,b,sizeof(SfMessage),1))return SF_DENIED;Endpoint*e=&endpoints[x->endpoint-1];
  if(e->count){
   unsigned idx=e->head;SfMessage*m=&e->queue[idx];long sender=m->h.flags;int installed=-1;
   if(m->h.cap_count){
    unsigned ep=e->cap_ep[idx];
    /* 附带能力在发送后可能被撤销、端点重生或接收方槽位耗尽；安装失败时
     * 仍交付消息正文，但按 SF_REVOKED 清空能力，不静默降级。 */
    if(ep&&ep<=ENDPOINTS&&endpoints[ep-1].owner&&endpoints[ep-1].epoch==e->cap_epoch[idx])
     installed=install(t,ep-1,e->cap_rights[idx],e->cap_parent_pid[idx],e->cap_parent_slot[idx],e->cap_parent_generation[idx]);
    if(installed<0){m->status=SF_REVOKED;m->handle=0;m->h.cap_count=0;}
    else m->handle=(uint64_t)(unsigned)installed;
    e->cap_ep[idx]=0;
   }
   if(copy_out(t,b,m,sizeof(*m))){if(installed>=0)close_cap(&task_main(t)->caps[(installed&255)-1]);return SF_DENIED;}
   copied(&receive_bytes,sizeof(*m));e->head=(e->head+1)%8;e->count--;return sender;
  }
  if(c&&ticks*10000000ull>=c)return SF_TIMEOUT;
  t->state=T_BLOCKED;t->wait_endpoint=x->endpoint;t->recv_buffer=b;t->wait_deadline=c;return 0;
 }
 return SF_INVALID;
}
