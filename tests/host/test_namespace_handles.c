#include <stdio.h>
#include "../../services/namespace_handles.h"
static SfNsHandleMap map,saved;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("NS HANDLES FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int add(uint32_t owner,uint32_t remote){SfNsHandleReservation r;int status=sf_nsh_prepare(&map,owner,&r);return status?status:sf_nsh_commit(&map,&r,remote);}
int main(void){
 sf_nsh_init(&map);SfNsHandleReservation r,other;uint32_t remote=0xdeadbeef;
 CHECK(sf_nsh_prepare(0,1,&r)==SF_INVALID);
 CHECK(sf_nsh_lookup(0,1,257,&remote)==SF_INVALID);
 CHECK(sf_nsh_lookup(&map,1,257,0)==SF_INVALID);
 CHECK(sf_nsh_commit(0,&r,257)==SF_INVALID);
 CHECK(sf_nsh_commit(&map,0,257)==SF_INVALID);
 CHECK(sf_nsh_prepare(&map,0,&r)==SF_INVALID);CHECK(sf_nsh_prepare(&map,1,0)==SF_INVALID);
 saved=map;CHECK(!sf_nsh_prepare(&map,1,&r));CHECK(!memcmp(&map,&saved,sizeof(map)));
 CHECK(!sf_nsh_prepare(&map,42,&other));
 uint64_t bad[]={0,1,255,256,257ull|(1ull<<32),0x80000101ull,UINT64_MAX};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){saved=map;CHECK(sf_nsh_commit(&map,&r,bad[i])==SF_BAD_FORMAT);CHECK(!memcmp(&map,&saved,sizeof(map)));}
 int first=sf_nsh_commit(&map,&r,257);CHECK(first>0);
 saved=map;CHECK(sf_nsh_commit(&map,&other,258)==SF_STALE_HANDLE);CHECK(!memcmp(&map,&saved,sizeof(map)));
 CHECK(!sf_nsh_lookup(&map,1,(uint32_t)first,&remote)&&remote==257);
 remote=0xdeadbeef;CHECK(sf_nsh_lookup(&map,42,(uint32_t)first,&remote)==SF_STALE_HANDLE&&remote==0xdeadbeef);
 CHECK(sf_nsh_lookup(&map,1,(uint32_t)first|(1ull<<32),&remote)==SF_STALE_HANDLE);
 CHECK(sf_nsh_lookup(&map,1,(uint32_t)first^256,&remote)==SF_STALE_HANDLE);
 const uint64_t malformed[]={0,1,128,129,255,256,385,0x80000101ull,UINT32_MAX,UINT64_MAX};
 for(unsigned i=0;i<sizeof(malformed)/sizeof(*malformed);i++){
  remote=0xdeadbeef;saved=map;
  CHECK(sf_nsh_lookup(&map,1,malformed[i],&remote)==SF_STALE_HANDLE);
  CHECK(remote==0xdeadbeef);CHECK(!memcmp(&map,&saved,sizeof(map)));
 }
 saved=map;CHECK(add(2,257)==SF_BAD_FORMAT);CHECK(!memcmp(&map,&saved,sizeof(map)));
 int second=add(42,258);CHECK(second>0&&second!=first);
 CHECK(sf_nsh_handoff(&map,1,(uint32_t)first,0)==SF_INVALID);
 CHECK(!sf_nsh_handoff(&map,1,(uint32_t)first,43));CHECK(sf_nsh_forget(&map,1,(uint32_t)first)==SF_STALE_HANDLE);
 sf_nsh_reap(&map,1);CHECK(!sf_nsh_lookup(&map,43,(uint32_t)first,&remote)&&remote==257);
 sf_nsh_reap(&map,42);CHECK(sf_nsh_lookup(&map,42,(uint32_t)second,&remote)==SF_STALE_HANDLE);
 CHECK(!sf_nsh_forget(&map,43,(uint32_t)first));int replacement=add(43,257);CHECK(replacement>0&&replacement!=first);
 CHECK(sf_nsh_lookup(&map,43,(uint32_t)first,&remote)==SF_STALE_HANDLE);
 /* A stale reservation cannot commit after a different owner used and then
  * released its slot, even within the same service incarnation. */
 CHECK(!sf_nsh_prepare(&map,8,&r));int interim=add(9,260);CHECK(interim>0);
 CHECK(!sf_nsh_forget(&map,9,(uint32_t)interim));saved=map;
 CHECK(sf_nsh_commit(&map,&r,261)==SF_STALE_HANDLE);CHECK(!memcmp(&map,&saved,sizeof(map)));
 CHECK(!sf_nsh_prepare(&map,9,&r));sf_nsh_invalidate(&map);
 saved=map;CHECK(sf_nsh_commit(&map,&r,259)==SF_STALE_HANDLE);CHECK(!memcmp(&map,&saved,sizeof(map)));
 CHECK(sf_nsh_lookup(&map,43,(uint32_t)replacement,&remote)==SF_STALE_HANDLE);
 /* An independent history of issued client IDs must stay unique even though
  * the restarted peer reuses exactly the same remote integer every time. */
 uint32_t issued[1000];
 for(unsigned epoch=0;epoch<1000;epoch++){
  int local=add(0x80000001u,257);CHECK(local>0);
  for(unsigned j=0;j<epoch;j++)CHECK((uint32_t)local!=issued[j]);
  CHECK(!sf_nsh_lookup(&map,0x80000001u,(uint32_t)local,&remote)&&remote==257);
  issued[epoch]=(uint32_t)local;sf_nsh_invalidate(&map);
  CHECK(sf_nsh_lookup(&map,0x80000001u,issued[epoch],&remote)==SF_STALE_HANDLE);
 }
 int active[SF_NS_HANDLE_MAX];
 for(unsigned i=0;i<SF_NS_HANDLE_MAX;i++){active[i]=add(i+1,257+i);CHECK(active[i]>0);}
 saved=map;CHECK(sf_nsh_prepare(&map,900,&r)==SF_NO_SPACE);CHECK(!memcmp(&map,&saved,sizeof(map)));
 for(unsigned i=0;i<SF_NS_HANDLE_MAX;i++){CHECK(!sf_nsh_lookup(&map,i+1,(uint32_t)active[i],&remote)&&remote==257+i);CHECK(!sf_nsh_forget(&map,i+1,(uint32_t)active[i]));}
 for(unsigned i=0;i<SF_NS_HANDLE_MAX;i++)map.handles[i].generation=SF_NSC_GENERATION_MAX;
 CHECK(sf_nsh_prepare(&map,1,&r)==SF_NO_SPACE);sf_nsh_invalidate(&map);CHECK(sf_nsh_prepare(&map,1,&r)==SF_NO_SPACE);
 /* The last positive 31-bit generation is usable exactly once; afterwards
  * the slot retires permanently rather than aliasing its first occupant. */
 map.handles[SF_NS_HANDLE_MAX-1].generation=SF_NSC_GENERATION_MAX-1;
 int terminal=add(77,257);CHECK(terminal==0x7fffff80);
 CHECK(!sf_nsh_forget(&map,77,(uint32_t)terminal));
 CHECK(sf_nsh_prepare(&map,77,&r)==SF_NO_SPACE);
 map.epoch=UINT64_MAX;map.handles[7].generation=0;CHECK(!sf_nsh_prepare(&map,1,&r));
 sf_nsh_invalidate(&map);CHECK(sf_nsh_prepare(&map,1,&other)==SF_NO_SPACE);CHECK(sf_nsh_commit(&map,&r,257)==SF_STALE_HANDLE);
 printf("Namespace restart handle-map tests passed: %u assertions\n",checks);return 0;
}
