#include <stdio.h>
#include "supervisor.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 SfSupervisor p={0},before=p;
 CHECK(sf_supervisor_init(&p,0,25,1000)==SF_INVALID);CHECK(!memcmp(&p,&before,sizeof(p)));
 CHECK(sf_supervisor_init(&p,17,25,1000)==SF_INVALID);CHECK(sf_supervisor_init(&p,3,0,1000)==SF_INVALID);CHECK(sf_supervisor_init(&p,3,25,0)==SF_INVALID);
 uint32_t seed=0x51f0a91;
 for(unsigned trace=0;trace<1000;trace++){
  seed=1664525u*seed+1013904223u;unsigned limit=1+(seed%16);uint64_t delay=1+((seed>>8)%100),now=1000;
  CHECK(!sf_supervisor_init(&p,limit,delay,1000));sf_supervisor_enable(&p,now,1);CHECK(p.state==SF_SUP_RUNNING&&!p.attempts);
  for(unsigned i=0;i<limit;i++){
   CHECK(!sf_supervisor_failure(&p,now));CHECK(p.state==SF_SUP_WAITING&&p.attempts==i&&p.next_tick==now+delay);
   before=p;CHECK(sf_supervisor_begin(&p,p.next_tick-1)==SF_WOULD_BLOCK);CHECK(!memcmp(&p,&before,sizeof(p)));
   now=p.next_tick;CHECK(!sf_supervisor_begin(&p,now));CHECK(p.state==SF_SUP_STARTING&&p.attempts==i+1);
   before=p;CHECK(sf_supervisor_begin(&p,now)==SF_INVALID);CHECK(!memcmp(&p,&before,sizeof(p)));
   CHECK(!sf_supervisor_started(&p,now));CHECK(p.state==SF_SUP_RUNNING&&p.attempts==i+1);now++;
  }
  CHECK(!sf_supervisor_failure(&p,now));CHECK(p.state==SF_SUP_BLOCKED&&p.attempts==limit);
  before=p;CHECK(sf_supervisor_begin(&p,UINT64_MAX)==SF_INVALID);CHECK(!memcmp(&p,&before,sizeof(p)));
  sf_supervisor_enable(&p,now,0);CHECK(p.state==SF_SUP_WAITING&&!p.attempts);CHECK(!sf_supervisor_begin(&p,now));CHECK(!sf_supervisor_started(&p,now));
  sf_supervisor_disable(&p);CHECK(p.state==SF_SUP_STOPPED&&!p.attempts);before=p;CHECK(sf_supervisor_failure(&p,now)==SF_INVALID);CHECK(!memcmp(&p,&before,sizeof(p)));
 }
 CHECK(!sf_supervisor_init(&p,1,25,1000));sf_supervisor_enable(&p,0,0);CHECK(!sf_supervisor_begin(&p,0));CHECK(!sf_supervisor_started(&p,10));
 before=p;CHECK(!sf_supervisor_failure(&p,1009));CHECK(p.state==SF_SUP_BLOCKED&&p.attempts==1);
 p=before;CHECK(!sf_supervisor_failure(&p,1010));CHECK(p.state==SF_SUP_WAITING&&!p.attempts);
 p=before;CHECK(!sf_supervisor_failure(&p,9));CHECK(p.state==SF_SUP_BLOCKED&&p.attempts==1);
 CHECK(!sf_supervisor_init(&p,1,25,1000));sf_supervisor_enable(&p,UINT64_MAX-10,1);
 CHECK(!sf_supervisor_failure(&p,UINT64_MAX-10));CHECK(p.next_tick==UINT64_MAX);CHECK(sf_supervisor_begin(&p,UINT64_MAX-1)==SF_WOULD_BLOCK);
 CHECK(!sf_supervisor_begin(&p,UINT64_MAX));CHECK(!sf_supervisor_failure(&p,UINT64_MAX));CHECK(p.state==SF_SUP_BLOCKED);
 printf("SUPERVISOR PASS: %u checks; 1000 bounded restart traces, exact stable boundary, backoff, explicit stop/retry and saturating deadlines\n",checks);return 0;
}
