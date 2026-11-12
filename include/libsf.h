#ifndef LIBSF_H
#define LIBSF_H
#include "sf.h"
static inline long sf_sys(long call,uint64_t a,uint64_t b,uint64_t c){register uint64_t rax __asm__("rax")=call;__asm__ volatile("int $0x80":"+a"(rax):"D"(a),"S"(b),"d"(c):"memory","cc");return(long)rax;}
static inline void sf_yield(void){sf_sys(SYS_YIELD,0,0,0);}
static inline uint64_t sf_ticks(void){return sf_sys(SYS_TICKS,0,0,0);}
static inline long sf_task_cap_usage(uint64_t pid){return sf_sys(SYS_TASK_CAP_LIMIT,pid,0,SF_LIMIT_USED);}
static inline long sf_spawn_limited(const SfLoadPlan*plan,uint64_t pages){return sf_sys(SYS_SPAWN_LIMITED,(uint64_t)plan,pages,0);}
static inline long sf_task_page_limit(uint64_t pid,uint64_t pages,uint64_t op){return sf_sys(SYS_TASK_PAGE_LIMIT,pid,pages,op);}
static inline long sf_task_page_usage(uint64_t pid){return sf_task_page_limit(pid,0,SF_LIMIT_USED);}
static inline long sf_ipc_stats(SfIpcStats*out){return sf_sys(SYS_IPC_STATS,(uint64_t)out,sizeof(*out),0);}
static inline long sf_thread_create(void*entry,uint64_t argument){SfThreadRequest r={sizeof(r),0,(uint64_t)entry,argument};return sf_sys(SYS_THREAD_CREATE,(uint64_t)&r,0,0);}
static inline long sf_thread_limit(uint64_t pid,uint64_t limit,uint64_t op){return sf_sys(SYS_THREAD_LIMIT,pid,limit,op);}
static inline long sf_task_cpu_budget(uint64_t pid,uint64_t ticks,uint64_t op){return sf_sys(SYS_TASK_CPU_BUDGET,pid,ticks,op);}
static inline void sf_log(const char*s){sf_sys(SYS_LOG,(uint64_t)s,strlen(s),0);}
static inline long sf_send(uint32_t h,SfMessage*m){return sf_sys(SYS_SEND,h,(uint64_t)m,0);}
static inline int64_t sf_send_cancel(uint64_t h,uint64_t id){return sf_sys(SYS_SEND_CANCEL,h,id,0);}
static inline long sf_recv(uint32_t h,SfMessage*m,uint64_t deadline){return sf_sys(SYS_RECV,h,(uint64_t)m,deadline);}
static inline long sf_ep_create(void){return sf_sys(SYS_EP_CREATE,0,0,0);}
static inline long sf_cap_close(uint64_t h){return sf_sys(SYS_CAP_CLOSE,h,0,0);}
static inline long sf_cap_derive(uint64_t h,uint64_t rights){return sf_sys(SYS_CAP_DERIVE,h,rights,0);}
static inline long sf_cap_revoke(uint64_t h){return sf_sys(SYS_CAP_REVOKE,h,0,0);}
/* 附带能力发送：cap_count=1 时 m->handle 被内核解释为要转移的能力句柄。
 * 接收方在 RECV 时得到同权的新句柄，也可改用 sf_send 保持 cap_count=0。 */
static inline long sf_send_cap(uint32_t h,SfMessage*m,uint64_t cap){m->h.cap_count=1;m->handle=cap;return sf_sys(SYS_SEND,h,(uint64_t)m,0);}
static inline uint64_t sf_deadline(unsigned ms){return sf_ticks()*10000000ull+(uint64_t)ms*1000000;}
/* 匿名堆与共享内存：addr=0 时由内核在 SF_HEAP 窗口内选择基址。
 * 共享内存固定映射到 SF_SHM 槽位；创建者可写，被授权者只读。 */
static inline long sf_mmap(uint64_t addr,uint64_t bytes){return sf_sys(SYS_MMAP,addr,bytes,0);}
static inline long sf_munmap(uint64_t addr,uint64_t bytes){return sf_sys(SYS_MUNMAP,addr,bytes,0);}
static inline long sf_shm_create(uint64_t bytes){return sf_sys(SYS_SHM_CREATE,bytes,0,0);}
static inline long sf_shm_map(uint64_t id){return sf_sys(SYS_SHM_MAP,id,0,0);}
static inline long sf_shm_grant(uint64_t id,uint64_t pid){return sf_sys(SYS_SHM_GRANT,id,pid,0);}
static inline long sf_shm_close(uint64_t id){return sf_sys(SYS_SHM_CLOSE,id,0,0);}
static inline void sf_message(SfMessage*m,unsigned op,uint64_t id){memset(m,0,sizeof(*m));m->h.size=40;m->h.protocol_major=1;m->h.opcode=op;m->h.request_id=id;m->h.payload_bytes=sizeof(*m)-40;m->h.deadline_ns=sf_deadline(3000);}
void sf_number(uint64_t);void sf_assert(int,const char*);
#endif
