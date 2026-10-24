#include "supervisor.h"
int sf_supervisor_init(SfSupervisor*p,uint32_t attempts,uint64_t retry,uint64_t stable){
 if(!p||!attempts||attempts>16||!retry||!stable)return SF_INVALID;
 memset(p,0,sizeof(*p));p->max_attempts=attempts;p->retry_ticks=retry;p->stable_ticks=stable;return 0;
}
void sf_supervisor_enable(SfSupervisor*p,uint64_t now,int running){
 p->attempts=0;p->state=running?SF_SUP_RUNNING:SF_SUP_WAITING;p->running_since=now;p->next_tick=now;
}
void sf_supervisor_disable(SfSupervisor*p){p->state=SF_SUP_STOPPED;p->attempts=0;p->next_tick=p->running_since=0;}
int sf_supervisor_failure(SfSupervisor*p,uint64_t now){
 if(p->state!=SF_SUP_RUNNING&&p->state!=SF_SUP_STARTING)return SF_INVALID;
 if(p->state==SF_SUP_RUNNING&&now>=p->running_since&&now-p->running_since>=p->stable_ticks)p->attempts=0;
 if(p->attempts>=p->max_attempts){p->state=SF_SUP_BLOCKED;p->next_tick=0;return 0;}
 p->state=SF_SUP_WAITING;p->next_tick=p->retry_ticks>UINT64_MAX-now?UINT64_MAX:now+p->retry_ticks;return 0;
}
int sf_supervisor_begin(SfSupervisor*p,uint64_t now){
 if(p->state!=SF_SUP_WAITING||p->attempts>=p->max_attempts)return SF_INVALID;
 if(now<p->next_tick)return SF_WOULD_BLOCK;p->attempts++;p->state=SF_SUP_STARTING;return 0;
}
int sf_supervisor_started(SfSupervisor*p,uint64_t now){
 if(p->state!=SF_SUP_STARTING)return SF_INVALID;p->state=SF_SUP_RUNNING;p->running_since=now;return 0;
}
