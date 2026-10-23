#include <stdio.h>
#include "manager.h"
#include "namespace.h"
static SfNamespace ns;static SfManager manager;static unsigned count;
#define CHECK(x) do{count++;if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 int b=sfmod_add(&manager,"B",0,0);CHECK(b>=0);CHECK(!sfmod_start(&manager,b));int a=sfmod_add(&manager,"A",1u<<b,0);CHECK(a>=0);CHECK(!sfmod_start(&manager,a));CHECK(!sfmod_prepare(&manager,b));CHECK(sfmod_acquire(&manager,b,RES_OPEN)==SF_DRAINING);CHECK(sfmod_stop(&manager,b)==SF_DEPENDENCY);CHECK(!sfmod_cancel(&manager,b));
 CHECK(!sfmod_prepare(&manager,a));CHECK(!sfmod_stop(&manager,a));CHECK(!sfmod_unload(&manager,a));
 for(unsigned r=0;r<RES_COUNT;r++){CHECK(!sfmod_acquire(&manager,b,r));CHECK(!sfmod_prepare(&manager,b));CHECK(sfmod_stop(&manager,b)==(r==RES_LEASE?SF_DEPENDENCY:SF_BUSY));CHECK(!sfmod_cancel(&manager,b));CHECK(!sfmod_release(&manager,b,r));CHECK(sfmod_release(&manager,b,r)==SF_INVALID);}
 uint32_t old=manager.modules[b].generation;CHECK(!sfmod_prepare(&manager,b));CHECK(!sfmod_stop(&manager,b));CHECK(!sfmod_unload(&manager,b));int newer=sfmod_add(&manager,"B",0,0);CHECK(newer>=0&&manager.modules[newer].generation>old);
 int pinned=sfmod_add(&manager,"init",0,1);CHECK(!sfmod_start(&manager,pinned));CHECK(sfmod_prepare(&manager,pinned)==SF_DENIED);
 uint32_t cycle[]={2,4,1};char chain[64];CHECK(sfmod_cycle(cycle,3,chain,sizeof(chain))==SF_DEPENDENCY);CHECK(!strcmp(chain,"A>B>C>A"));uint32_t dag[]={2,4,0};CHECK(!sfmod_cycle(dag,3,chain,sizeof(chain)));
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/home",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/home/a",NODE_FILE,0));CHECK(!sf_ns_create(&ns,"/pipe",NODE_PIPE,0));CHECK(!sf_ns_create(&ns,"/rom",NODE_FILE,1));CHECK(sf_ns_open(&ns,1,"/","/rom",2)==SF_READ_ONLY);
 CHECK(sf_ns_open(&ns,1,"/home","/",1)==SF_DENIED);CHECK(sf_ns_open(&ns,1,"/home","/home-other",1)==SF_DENIED);CHECK(sf_ns_open(&ns,1,"/","/home/../rom",1)==SF_INVALID);CHECK(sf_ns_create(&ns,"/\xc0\xaf",NODE_FILE,0)==SF_INVALID);
 int h=sf_ns_open(&ns,1,"/","/home/a",3);CHECK(h>0);CHECK(sf_ns_write(&ns,1,h,"abcdef",6)==6);CHECK(!sf_ns_seek(&ns,1,h,0));char out[2048]={0};CHECK(sf_ns_read(&ns,2,h,out,6)==SF_STALE_HANDLE);CHECK(sf_ns_read(&ns,1,h,out,2)==2&&!memcmp(out,"ab",2));int dup=sf_ns_dup(&ns,1,h);CHECK(dup>0&&dup!=h);CHECK(sf_ns_read(&ns,1,dup,out,2)==2&&!memcmp(out,"cd",2));int independent=sf_ns_open(&ns,1,"/","/home/a",1);CHECK(sf_ns_read(&ns,1,independent,out,2)==2&&!memcmp(out,"ab",2));CHECK(!sf_ns_close(&ns,1,h));CHECK(sf_ns_read(&ns,1,h,out,1)==SF_STALE_HANDLE);CHECK(sf_ns_read(&ns,1,dup,out,10)==2);CHECK(!sf_ns_read(&ns,1,dup,out,10));
 int rd=sf_ns_open(&ns,1,"/","/pipe",1),wr=sf_ns_open(&ns,1,"/","/pipe",2);CHECK(rd>0&&wr>0);CHECK(sf_ns_read(&ns,1,rd,out,1)==SF_WOULD_BLOCK);CHECK(sf_ns_seek(&ns,1,rd,0)==SF_NOT_SEEKABLE);CHECK(sf_ns_write(&ns,1,wr,out,sizeof(out))==1024);CHECK(!sf_ns_write(&ns,1,wr,out,0));CHECK(sf_ns_write(&ns,1,wr,out,1)==SF_WOULD_BLOCK);CHECK(sf_ns_poll(&ns,1,rd)&1);CHECK(sf_ns_read(&ns,1,rd,out,100)==100);CHECK(sf_ns_write(&ns,1,wr,out,200)==100);CHECK(!sf_ns_close(&ns,1,wr));CHECK(sf_ns_read(&ns,1,rd,out,sizeof(out))==1024);CHECK(!sf_ns_read(&ns,1,rd,out,1));CHECK(sf_ns_poll(&ns,1,rd)&4);
 int dir=sf_ns_open(&ns,1,"/","/home",1);CHECK(sf_ns_enumerate(&ns,1,dir,out,sizeof(out))==2&&!memcmp(out,"a\n",2));sf_ns_reap(&ns,1);CHECK(sf_ns_read(&ns,1,independent,out,1)==SF_STALE_HANDLE);
 printf("Service tests passed: %u assertions\n",count);return 0;
}
