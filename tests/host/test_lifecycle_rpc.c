#include "lifecycle_rpc.h"
#include <stdio.h>
#include <string.h>
static unsigned checks;
#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);return 1;}checks++;}while(0)
static SfMessage req(unsigned op,unsigned slot,uint64_t id){
 SfMessage m;memset(&m,0,sizeof(m));m.h.size=40;m.h.protocol_major=1;m.h.opcode=op;m.h.request_id=id;m.h.payload_bytes=sizeof(m)-40;m.mode=slot;return m;
}
int main(void){
 SfManager m={0};
 int a=sfmod_add(&m,"a.mod",0,0);CHECK(a==0&&m.modules[a].state==MOD_VERIFIED);
 SfMessage q=req(OP_MODULE_STATUS,a,1);CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));
 CHECK(!q.status&&q.length==strlen("a.mod")+1&&!strcmp((char*)q.data,"a.mod")&&q.mode==MOD_VERIFIED&&q.offset==m.modules[a].generation);
 /* 非根调用者按内核提供的身份拒绝，不泄露槽位信息。 */
 q=req(OP_MODULE_STATUS,a,2);CHECK(!sf_lifecycle_dispatch(&m,7,1,&q));CHECK(q.status==SF_DENIED&&q.length==0);
 q=req(OP_MODULE_START,a,3);CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(!q.status&&m.modules[a].state==MOD_ACTIVE);
 q=req(OP_MODULE_START,a,4);CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(q.status==SF_INVALID&&m.modules[a].state==MOD_ACTIVE);
 q=req(OP_MODULE_LIST,a,5);q.length=16;CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(!q.status&&q.length==16&&q.data[a]==MOD_ACTIVE);
 q=req(OP_MODULE_LIST,a,6);q.length=15;CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(q.status==SF_NO_SPACE);
 /* 租约与打开资源经 RPC 记账，重复释放不产生负计数。 */
 q=req(OP_MODULE_ACQUIRE,a,7);q.offset=RES_OPEN;CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(!q.status&&m.modules[a].resources[RES_OPEN]==1);
 q=req(OP_MODULE_ACQUIRE,a,8);q.offset=RES_COUNT;CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(q.status==SF_INVALID);
 q=req(OP_MODULE_RELEASE,a,9);q.offset=RES_OPEN;CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(!q.status&&!m.modules[a].resources[RES_OPEN]);
 q=req(OP_MODULE_RELEASE,a,10);q.offset=RES_OPEN;CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(q.status==SF_INVALID);
 q=req(OP_MODULE_ACQUIRE,a,11);q.offset=RES_LEASE;CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(!q.status);
 q=req(OP_MODULE_PREPARE,a,12);CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(!q.status&&m.modules[a].state==MOD_DRAINING);
 q=req(OP_MODULE_STOP,a,13);CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(q.status==SF_DEPENDENCY&&m.modules[a].state==MOD_DRAINING);
 q=req(OP_MODULE_CANCEL,a,14);CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(!q.status&&m.modules[a].state==MOD_ACTIVE);
 q=req(OP_MODULE_PREPARE,a,15);CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(!q.status&&m.modules[a].state==MOD_DRAINING);
 q=req(OP_MODULE_RELEASE,a,16);q.offset=RES_LEASE;CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(!q.status);
 q=req(OP_MODULE_STOP,a,17);CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(!q.status&&m.modules[a].state==MOD_STOPPED);
 q=req(OP_MODULE_UNLOAD,a,18);CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(!q.status&&m.modules[a].state==MOD_UNLOADED);
 q=req(OP_MODULE_STATUS,99,19);CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(q.status==SF_NOT_FOUND);
 q=req(OP_MODULE_STOP,99,20);CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(q.status==SF_INVALID);
 /* 消息头非法时不分发、不回复，调用方不能借越界尺寸改变状态。 */
 q=req(OP_MODULE_STATUS,a,21);q.h.size=41;CHECK(sf_lifecycle_dispatch(&m,1,1,&q)==SF_INVALID);
 q=req(OP_MODULE_STATUS,a,22);q.h.protocol_minor=1;CHECK(sf_lifecycle_dispatch(&m,1,1,&q)==SF_INVALID);
 q=req(999,a,23);CHECK(!sf_lifecycle_dispatch(&m,1,1,&q));CHECK(q.status==SF_INVALID);
 printf("Lifecycle RPC tests passed: %u assertions\n",checks);return 0;
}
