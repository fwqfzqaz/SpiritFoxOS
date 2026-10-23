#include "manager.h"
static int registered(const SfModule*x){return x->generation&&x->state!=MOD_DISCOVERED&&x->state!=MOD_UNLOADED;}
int sfmod_add(SfManager*m,const char*id,uint32_t deps,int pinned){
 if(!id[0]||strlen(id)>=64)return SF_INVALID;
 if(deps>>16)return SF_DEPENDENCY;
 for(unsigned j=0;j<16;j++)if((deps&(1u<<j))&&!registered(&m->modules[j]))return SF_DEPENDENCY;
 if(m->next_generation==UINT32_MAX)return SF_NO_SPACE;
 for(unsigned i=0;i<16;i++)if(m->modules[i].state==MOD_DISCOVERED||m->modules[i].state==MOD_UNLOADED){
  SfModule*x=&m->modules[i];memset(x,0,sizeof(*x));memcpy(x->id,id,strlen(id)+1);
  x->generation=++m->next_generation;x->dependencies=deps;x->pinned=pinned;x->state=MOD_VERIFIED;
  for(unsigned j=0;j<16;j++)if(deps&(1u<<j))x->dependency_generations[j]=m->modules[j].generation;
  return i;
 }
 return SF_NO_SPACE;
}
int sfmod_start(SfManager*m,unsigned i){
 if(i>=16)return SF_INVALID;SfModule*x=&m->modules[i];
 if(x->state!=MOD_VERIFIED&&x->state!=MOD_STOPPED)return SF_INVALID;
 if(x->dependencies>>16)return SF_DEPENDENCY;
 /* All checks precede state changes and all provider increments. */
 for(unsigned j=0;j<16;j++)if(x->dependencies&(1u<<j)){
  SfModule*p=&m->modules[j];
  if(j==i||p->state!=MOD_ACTIVE||p->generation!=x->dependency_generations[j])return SF_DEPENDENCY;
  if(p->resources[RES_LEASE]==UINT32_MAX)return SF_NO_SPACE;
 }
 x->state=MOD_RESOLVED;x->state=MOD_STAGED;x->state=MOD_INITIALIZED;
 for(unsigned j=0;j<16;j++)if(x->dependencies&(1u<<j))m->modules[j].resources[RES_LEASE]++;
 x->state=MOD_ACTIVE;return 0;
}
int sfmod_acquire(SfManager*m,unsigned i,unsigned r){if(i>=16||r>=RES_COUNT)return SF_INVALID;SfModule*x=&m->modules[i];if(x->state==MOD_QUIESCING||x->state==MOD_DRAINING)return SF_DRAINING;if(x->state!=MOD_ACTIVE)return SF_STALE_HANDLE;if(x->resources[r]==UINT32_MAX)return SF_NO_SPACE;x->resources[r]++;return 0;}
int sfmod_release(SfManager*m,unsigned i,unsigned r){if(i>=16||r>=RES_COUNT||!m->modules[i].resources[r])return SF_INVALID;m->modules[i].resources[r]--;return 0;}
int sfmod_prepare(SfManager*m,unsigned i){if(i>=16)return SF_INVALID;SfModule*x=&m->modules[i];if(x->pinned)return SF_DENIED;if(x->state!=MOD_ACTIVE)return SF_INVALID;x->state=MOD_QUIESCING;x->state=MOD_DRAINING;return 0;}
int sfmod_cancel(SfManager*m,unsigned i){if(i>=16||m->modules[i].state!=MOD_DRAINING)return SF_INVALID;m->modules[i].state=MOD_ACTIVE;return 0;}
int sfmod_stop(SfManager*m,unsigned i){
 if(i>=16||m->modules[i].state!=MOD_DRAINING)return SF_INVALID;SfModule*x=&m->modules[i];
 for(unsigned r=0;r<RES_COUNT;r++)if(x->resources[r])return r==RES_LEASE?SF_DEPENDENCY:SF_BUSY;
 if(x->dependencies>>16)return SF_DEPENDENCY;
 /* A malformed ledger must not partially release the earlier providers. */
 for(unsigned j=0;j<16;j++)if(x->dependencies&(1u<<j)){
  SfModule*p=&m->modules[j];
  if(!registered(p)||p->generation!=x->dependency_generations[j])return SF_DEPENDENCY;
  if(!p->resources[RES_LEASE])return SF_INVALID;
 }
 for(unsigned j=0;j<16;j++)if(x->dependencies&(1u<<j))m->modules[j].resources[RES_LEASE]--;
 x->state=MOD_STOPPED;return 0;
}
int sfmod_unload(SfManager*m,unsigned i){if(i>=16||m->modules[i].state!=MOD_STOPPED)return SF_INVALID;m->modules[i].state=MOD_UNLOADED;return 0;}
static int visit(const uint32_t*deps,unsigned n,unsigned i,uint8_t*marks,char*chain,size_t size,unsigned depth){if(marks[i]==1){if(depth+2<size){chain[depth]='A'+i;chain[depth+1]=0;}return 1;}if(marks[i]==2)return 0;marks[i]=1;if(depth+2>=size)return 1;chain[depth]='A'+i;chain[depth+1]='>';chain[depth+2]=0;
 for(unsigned j=0;j<n;j++)if((deps[i]&(1u<<j))&&visit(deps,n,j,marks,chain,size,depth+2))return 1;marks[i]=2;chain[depth]=0;return 0;}
int sfmod_cycle(const uint32_t*deps,unsigned n,char*chain,size_t size){if(n>16||size<4)return SF_INVALID;uint8_t marks[16]={0};for(unsigned i=0;i<n;i++){if(deps[i]>>n)return SF_DEPENDENCY;if(visit(deps,n,i,marks,chain,size,0))return SF_DEPENDENCY;}chain[0]=0;return 0;}
