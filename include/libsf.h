#ifndef LIBSF_H
#define LIBSF_H
#include "sf.h"
static inline long sf_sys(long call,uint64_t a,uint64_t b,uint64_t c){register uint64_t rax __asm__("rax")=call;__asm__ volatile("int $0x80":"+a"(rax):"D"(a),"S"(b),"d"(c):"memory","cc");return(long)rax;}
static inline void sf_yield(void){sf_sys(SYS_YIELD,0,0,0);}
static inline uint64_t sf_ticks(void){return sf_sys(SYS_TICKS,0,0,0);}
static inline void sf_log(const char*s){sf_sys(SYS_LOG,(uint64_t)s,strlen(s),0);}
static inline long sf_send(uint32_t h,SfMessage*m){return sf_sys(SYS_SEND,h,(uint64_t)m,0);}
static inline int64_t sf_send_cancel(uint64_t h,uint64_t id){return sf_sys(SYS_SEND_CANCEL,h,id,0);}
static inline long sf_recv(uint32_t h,SfMessage*m,uint64_t deadline){return sf_sys(SYS_RECV,h,(uint64_t)m,deadline);}
static inline uint64_t sf_deadline(unsigned ms){return sf_ticks()*10000000ull+(uint64_t)ms*1000000;}
static inline void sf_message(SfMessage*m,unsigned op,uint64_t id){memset(m,0,sizeof(*m));m->h.size=40;m->h.protocol_major=1;m->h.opcode=op;m->h.request_id=id;m->h.payload_bytes=sizeof(*m)-40;m->h.deadline_ns=sf_deadline(3000);}
void sf_number(uint64_t);void sf_assert(int,const char*);
#endif
