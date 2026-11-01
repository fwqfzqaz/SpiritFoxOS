#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/kernel.h"
Task tasks[SF_MAX_TASKS];
static unsigned checks;
static uintptr_t addresses[SF_PAGE_LIMIT_MAX];
static uint8_t zero[4096],borrowed[4096] __attribute__((aligned(4096)));
#define CHECK(x) do{checks++;if(!(x)){printf("MEMORY QUOTA FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int order(const void*a,const void*b){uintptr_t x=*(const uintptr_t*)a,y=*(const uintptr_t*)b;return (x>y)-(x<y);}
int main(void){
 tasks[0].pid=1;tasks[0].page_limit=0;
 CHECK(!page_alloc(1)&&!tasks[0].page_count);
 tasks[0].page_limit=2;uint8_t*a=page_alloc(1),*b=page_alloc(1);
 CHECK(a&&b&&a!=b&&tasks[0].page_count==2&&!memcmp(a,zero,4096)&&!memcmp(b,zero,4096));
 memset(a,0xa5,4096);memset(b,0x5a,4096);CHECK(!page_alloc(1)&&tasks[0].page_count==2);
 tasks[1].pid=2;tasks[1].state=T_BLOCKED;tasks[1].page_limit=1;uint8_t*c=page_alloc(2);
 CHECK(c&&c!=a&&c!=b&&tasks[1].page_count==1);memset(c,0x3c,4096);
 tasks[0].state=T_DEAD;pages_free(1);CHECK(!tasks[0].page_count&&!memcmp(a,zero,4096)&&!memcmp(b,zero,4096));
 CHECK(!page_alloc(1)&&tasks[1].page_count==1&&c[0]==0x3c&&c[4095]==0x3c);
 CHECK(!page_alloc(0)&&!page_alloc(INT32_MAX+1u)&&!page_alloc(17));
 pages_free(0);pages_free(17);CHECK(tasks[1].page_count==1&&c[0]==0x3c);pages_free(2);
 /* Partial address-space construction is charged until task rollback frees it. */
 tasks[0].state=T_EMPTY;tasks[0].page_limit=1;CHECK(!space_new(1)&&tasks[0].page_count==1);
 pages_free(1);CHECK(!tasks[0].page_count);
 tasks[0].page_limit=6;tasks[0].cr3=space_new(1);CHECK(tasks[0].cr3&&tasks[0].page_count==2);
 a=page_alloc(1);CHECK(a&&!space_map(&tasks[0],SF_BASE,(uintptr_t)a,5)&&tasks[0].page_count==5);
 b=page_alloc(1);CHECK(b&&!space_map(&tasks[0],SF_BASE+4096,(uintptr_t)b,3)&&tasks[0].page_count==6);
 memset(borrowed,0x6b,sizeof(borrowed));CHECK(!space_map(&tasks[0],SF_BASE+8192,(uintptr_t)borrowed,1)&&tasks[0].page_count==6);
 CHECK(!page_alloc(1));pages_free(1);CHECK(!tasks[0].page_count&&borrowed[0]==0x6b&&borrowed[4095]==0x6b);
 /* Global exhaustion is independent of either task's configured ceiling. */
 tasks[0].page_limit=tasks[1].page_limit=SF_PAGE_LIMIT_MAX;tasks[0].state=T_READY;
 for(unsigned i=0;i<SF_PAGE_LIMIT_MAX;i++){
  a=page_alloc(1);CHECK(a&&tasks[0].page_count==i+1&&!memcmp(a,zero,4096));addresses[i]=(uintptr_t)a;memset(a,(i%251)+1,4096);
 }
 qsort(addresses,SF_PAGE_LIMIT_MAX,sizeof(addresses[0]),order);
 for(unsigned i=1;i<SF_PAGE_LIMIT_MAX;i++)CHECK(addresses[i]!=addresses[i-1]);
 CHECK(!page_alloc(1)&&!page_alloc(2)&&tasks[0].page_count==SF_PAGE_LIMIT_MAX&&!tasks[1].page_count);
 pages_free(1);CHECK(!tasks[0].page_count);a=page_alloc(2);CHECK(a&&!memcmp(a,zero,4096)&&tasks[1].page_count==1);pages_free(2);
 /* Independent small-budget/interleaving oracle; total <=64 avoids pool exhaustion. */
 uint32_t seed=0x7be319u;
 for(unsigned trial=0;trial<1000;trial++){
  uint8_t*held[2][32];unsigned used[2]={0,0},limits[2];
  for(unsigned i=0;i<2;i++){
   seed=seed*1664525u+1013904223u;limits[i]=trial==0?0:trial==1?32:seed%33;
   tasks[i].state=i?T_BLOCKED:T_READY;tasks[i].page_limit=limits[i];tasks[i].priority=3;tasks[i].cap_limit=17;
  }
  for(unsigned step=0;step<80;step++){
   seed=seed*1664525u+1013904223u;unsigned who=(seed>>17)&1;unsigned before=used[who];a=page_alloc(who+1);
   if(before<limits[who]){CHECK(a&&!memcmp(a,zero,4096));held[who][used[who]++]=a;memset(a,who+1,4096);}
   else CHECK(!a);
   for(unsigned i=0;i<2;i++)CHECK(tasks[i].page_count==used[i]&&tasks[i].page_limit==limits[i]&&tasks[i].priority==3&&tasks[i].cap_limit==17&&tasks[i].state==(i?T_BLOCKED:T_READY));
  }
  for(unsigned i=0;i<2;i++)for(unsigned j=0;j<used[i];j++)CHECK(held[i][j][0]==i+1&&held[i][j][4095]==i+1);
  tasks[0].state=T_DEAD;pages_free(1);CHECK(!tasks[0].page_count);
  for(unsigned j=0;j<used[1];j++)CHECK(held[1][j][0]==2&&held[1][j][4095]==2);
  pages_free(2);CHECK(!tasks[1].page_count);
 }
 tasks[0].pid=7;tasks[0].state=T_READY;tasks[0].page_limit=SF_PAGE_LIMIT_MAX;
 CHECK(!page_alloc(1));a=page_alloc(7);CHECK(a&&tasks[0].page_count==1&&!memcmp(a,zero,4096));pages_free(7);
 printf("MEMORY QUOTA PASS: %u checks; actual allocator, 1000 independent budget traces, pool/owner isolation, page tables, borrowed mapping, zeroing and death/reuse\n",checks);return 0;
}
