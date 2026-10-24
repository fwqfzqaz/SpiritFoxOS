#include <stdio.h>
#include "manager.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int replacement(void){
 SfManager m={0};
 for(unsigned iteration=0;iteration<1000;iteration++){
  int p=sfmod_add(&m,"provider",0,0);CHECK(p==0);CHECK(!sfmod_start(&m,p));
  int c=sfmod_add(&m,"consumer",1u<<p,0);CHECK(c==1);CHECK(!sfmod_start(&m,c));CHECK(m.modules[p].resources[RES_LEASE]==1);
  CHECK(!sfmod_prepare(&m,c));CHECK(!sfmod_stop(&m,c));CHECK(!m.modules[p].resources[RES_LEASE]);
  CHECK(!sfmod_prepare(&m,p));CHECK(!sfmod_stop(&m,p));CHECK(!sfmod_unload(&m,p));uint32_t old=m.modules[p].generation;
  int q=sfmod_add(&m,iteration&1?"provider":"replacement",0,0);CHECK(q==p&&m.modules[q].generation>old);CHECK(!sfmod_start(&m,q));
  CHECK(sfmod_start(&m,c)==SF_DEPENDENCY);CHECK(m.modules[c].state==MOD_STOPPED&&!m.modules[q].resources[RES_LEASE]);
  CHECK(!sfmod_unload(&m,c));c=sfmod_add(&m,"rebound-consumer",1u<<q,0);CHECK(c>=0);CHECK(!sfmod_start(&m,c));
  CHECK(!sfmod_prepare(&m,c));CHECK(!sfmod_stop(&m,c));CHECK(!m.modules[q].resources[RES_LEASE]);
  CHECK(!sfmod_unload(&m,c));CHECK(!sfmod_prepare(&m,q));CHECK(!sfmod_stop(&m,q));CHECK(!sfmod_unload(&m,q));
 }
 return 0;
}
static int validation(void){
 SfManager m={0},before=m;
 CHECK(sfmod_add(&m,"bad",1u<<16,0)==SF_DEPENDENCY);CHECK(!memcmp(&m,&before,sizeof(m)));
 CHECK(sfmod_add(&m,"empty",1u<<3,0)==SF_DEPENDENCY);CHECK(!memcmp(&m,&before,sizeof(m)));
 int p=sfmod_add(&m,"provider",0,0);CHECK(p==0);
 int c=sfmod_add(&m,"consumer",1u<<p,0);CHECK(c==1);before=m;
 CHECK(sfmod_start(&m,c)==SF_DEPENDENCY);CHECK(!memcmp(&m,&before,sizeof(m)));
 CHECK(!sfmod_start(&m,p));CHECK(!sfmod_start(&m,c));CHECK(!sfmod_prepare(&m,c));CHECK(!sfmod_stop(&m,c));
 CHECK(!sfmod_prepare(&m,p));CHECK(!sfmod_stop(&m,p));uint32_t generation=m.modules[p].generation;
 CHECK(!sfmod_start(&m,p));CHECK(m.modules[p].generation==generation);CHECK(!sfmod_start(&m,c));
 CHECK(!sfmod_prepare(&m,p));CHECK(sfmod_stop(&m,p)==SF_DEPENDENCY);
 CHECK(!sfmod_prepare(&m,c));CHECK(!sfmod_stop(&m,c));CHECK(!sfmod_stop(&m,p));CHECK(!sfmod_unload(&m,p));
 before=m;CHECK(sfmod_add(&m,"unloaded",1u<<p,0)==SF_DEPENDENCY);CHECK(!memcmp(&m,&before,sizeof(m)));
 m.next_generation=UINT32_MAX;before=m;CHECK(sfmod_add(&m,"wrap",0,0)==SF_NO_SPACE);CHECK(!memcmp(&m,&before,sizeof(m)));
 m=(SfManager){0};m.next_generation=UINT32_MAX-1;p=sfmod_add(&m,"last",0,0);CHECK(p==0&&m.modules[p].generation==UINT32_MAX);
 before=m;CHECK(sfmod_add(&m,"wrap",0,0)==SF_NO_SPACE);CHECK(!memcmp(&m,&before,sizeof(m)));
 return 0;
}
static int accounting(void){
 SfManager m={0};int a=sfmod_add(&m,"A",0,0),b=sfmod_add(&m,"B",0,0);CHECK(a==0&&b==1);
 CHECK(!sfmod_start(&m,a));CHECK(!sfmod_start(&m,b));int c=sfmod_add(&m,"C",(1u<<a)|(1u<<b),0);CHECK(c==2);
 m.modules[b].resources[RES_LEASE]=UINT32_MAX;SfManager before=m;
 CHECK(sfmod_start(&m,c)==SF_NO_SPACE);CHECK(!memcmp(&m,&before,sizeof(m)));
 m.modules[b].resources[RES_LEASE]=0;CHECK(!sfmod_start(&m,c));CHECK(m.modules[a].resources[RES_LEASE]==1&&m.modules[b].resources[RES_LEASE]==1);
 CHECK(!sfmod_prepare(&m,c));m.modules[b].resources[RES_LEASE]=0;before=m;
 CHECK(sfmod_stop(&m,c)==SF_INVALID);CHECK(!memcmp(&m,&before,sizeof(m)));
 m.modules[b].resources[RES_LEASE]=1;CHECK(!sfmod_stop(&m,c));CHECK(!m.modules[a].resources[RES_LEASE]&&!m.modules[b].resources[RES_LEASE]);
 CHECK(!sfmod_start(&m,c));CHECK(!sfmod_prepare(&m,c));CHECK(!sfmod_cancel(&m,c));
 CHECK(m.modules[a].resources[RES_LEASE]==1&&m.modules[b].resources[RES_LEASE]==1);
 CHECK(!sfmod_prepare(&m,c));CHECK(!sfmod_stop(&m,c));CHECK(!sfmod_unload(&m,c));
 for(unsigned r=0;r<RES_COUNT;r++){m.modules[a].resources[r]=UINT32_MAX;before=m;CHECK(sfmod_acquire(&m,a,r)==SF_NO_SPACE);CHECK(!memcmp(&m,&before,sizeof(m)));m.modules[a].resources[r]=0;CHECK(sfmod_release(&m,a,r)==SF_INVALID);}
 return 0;
}
int main(void){if(replacement()||validation()||accounting())return 1;printf("MANAGER PASS: %u checks; generation-bound dependencies, transactional lease accounting and 1000 replacement cycles\n",checks);return 0;}
