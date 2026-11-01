#include "sched_policy.h"
int sf_sched_pick(const SfSchedEntry*entries,unsigned count,int current,uint64_t now){
 if(!entries||!count||count>SF_MAX_TASKS||current< -1||current>=(int)count)return -1;
 int selected=-1;unsigned best=0;
 for(unsigned step=1;step<=count;step++){
  unsigned i=(current+(int)step)%(int)count;const SfSchedEntry*e=&entries[i];
  if(!e->ready||e->priority>SF_PRIORITY_MAX)continue;
  uint64_t age=now>=e->ready_since?now-e->ready_since:0,boost=age/SF_PRIORITY_AGING_TICKS;
  unsigned score=boost>=SF_PRIORITY_MAX-e->priority?SF_PRIORITY_MAX:e->priority+(unsigned)boost;
  /* Traversal starts after current: equal effective levels remain round robin. */
  if(selected<0||score>best){selected=(int)i;best=score;}
 }
 return selected;
}
