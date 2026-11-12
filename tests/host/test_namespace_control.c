#include <stdio.h>
#include "namespace_control.h"
#include "namespace_client.h"
#include "namespace_state.h"
static SfNsControl server;
static SfNamespace before;
static SfNsClient client;
static uint32_t peer=50;
static unsigned checks,exchanges,losses,leases,tamper;
static int alive=1;
#define CHECK(x) do{checks++;if(!(x)){printf("NS RPC FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static long console(const void*data,size_t bytes){(void)data;return(long)bytes;}
static long disk(unsigned index,uint64_t offset,void*out,size_t bytes){
 if(!alive)return SF_DEVICE_GONE;if(index!=7)return SF_INVALID;
 if(offset>=4)return 0;if(bytes>4-offset)bytes=(size_t)(4-offset);memcpy(out,&"disk"[offset],bytes);return(long)bytes;
}
static int opened(unsigned tag){if(tag!=1)return SF_STALE_HANDLE;leases++;return 0;}
static void closed(unsigned tag){if(tag==1&&leases)leases--;}
static long projection(unsigned index,uint64_t offset,void*out,size_t bytes){
 if(index!=9)return SF_NOT_FOUND;if(offset>=5)return 0;if(bytes>5-offset)bytes=(size_t)(5-offset);
 memcpy(out,&"live\n"[offset],bytes);return(long)bytes;
}
static int64_t exchange(void*context,SfMessage*m){
 (void)context;exchanges++;m->h.flags=1;sf_nsr_dispatch(&server,1,m);
 m->h.flags=peer;m->h.deadline_ns=0;m->h.cap_count=0;uint32_t sender=peer;
 switch(tamper){
  case 1:sender++;break;case 2:m->h.flags++;break;case 3:m->h.request_id++;break;
  case 4:m->h.opcode++;break;case 5:m->h.size--;break;case 6:m->h.protocol_major++;break;
  case 7:m->h.protocol_minor++;break;case 8:m->h.payload_bytes--;break;
  case 9:m->h.cap_count=1;break;case 10:m->h.deadline_ns=1;break;
  case 11:m->length=257;break;case 12:m->status=257;break;case 13:m->length++;break;
  case 14:m->mode++;break;case 15:m->handle++;break;case 16:m->offset++;break;
  case 17:m->status=SF_DEVICE_GONE;m->length=1;break;
  case 18:m->status=INT64_MIN;m->length=0;break;
  case 19:return SF_TIMEOUT;
 }
 return sender;
}
static void lost(void*context){(void)context;losses++;}
static int boot(void){
 const SfNsBackends backends={.console=console,.read=disk,.mount_open=opened,.mount_close=closed,.projection=projection};
 leases=0;alive=1;tamper=0;sf_nsr_init(&server,sf_ns_default(),&backends);peer++;
 return sf_nsc_connect(&client,peer,exchange,0,lost,SF_NS_B_CONSOLE|SF_NS_B_READ|SF_NS_B_MOUNT_OPEN|SF_NS_B_MOUNT_CLOSE|SF_NS_B_PROJECTION);
}
static void request(SfMessage*m,unsigned op){memset(m,0,sizeof(*m));m->h.size=40;m->h.protocol_major=1;m->h.flags=1;m->h.payload_bytes=sizeof(*m)-40;m->h.opcode=op;m->h.request_id=1;}
static void put32(uint8_t*p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static void entry(SfMessage*m,uint64_t token,uint32_t index,const char*path,unsigned type){
 request(m,OP_NS_MOUNT_ENTRY);m->offset=token;put32(m->data,index);put32(m->data+4,type);put32(m->data+8,1);
 size_t n=strlen(path)+1;memcpy(m->data+20,path,n);m->length=(uint32_t)(20+n);
}
int main(void){
 CHECK(!boot());CHECK(!sf_nsc_create(&client,"/tmp",NODE_DIR,0,0,0,0777));
 CHECK(!sf_nsc_create(&client,"/tmp/file",NODE_FILE,0,7,9,0640));
 CHECK(sf_nsc_open(&client,8,"/","/tmp/file",OPEN_READ,99,99)==SF_DENIED&&client.ready);
 int h=sf_nsc_open(&client,7,"/","/tmp/file",OPEN_READ|OPEN_WRITE,7,9);CHECK(h>0);
 const uint8_t data[]={0,1,3,0,255};CHECK(sf_nsc_write(&client,7,(uint32_t)h,data,sizeof(data))==sizeof(data));
 CHECK(sf_nsc_seek(&client,7,(uint32_t)h,0)==0);
 uint8_t out[260];memset(out,0xa5,sizeof(out));CHECK(sf_nsc_read(&client,7,(uint32_t)h,out,3,0,0)==3&&!memcmp(out,data,3)&&out[3]==0xa5);
 int duplicate=sf_nsc_dup(&client,7,(uint32_t)h);CHECK(duplicate>0&&duplicate!=h);
 CHECK(sf_nsc_read(&client,7,(uint32_t)duplicate,out,2,0,0)==2&&!memcmp(out,data+3,2));
 CHECK(sf_nsc_read(&client,7,(uint32_t)duplicate,out,2,0,1)==2&&!memcmp(out,data,2));
 SfFileStat stat;CHECK(!sf_nsc_stat(&client,7,(uint32_t)h,&stat)&&stat.offset==5&&stat.size==5);
 unsigned calls=exchanges;CHECK(sf_nsc_read(&client,7,(uint32_t)h,out,sizeof(((SfMessage*)0)->data)+1,0,0)==SF_INVALID);
 CHECK(sf_nsc_read(&client,7,(uint32_t)h|(1ull<<32),out,1,0,0)==SF_STALE_HANDLE&&exchanges==calls);
 CHECK(!sf_nsc_handoff(&client,7,(uint32_t)duplicate,8));
 CHECK(sf_nsc_read(&client,7,(uint32_t)duplicate,out,1,0,0)==SF_STALE_HANDLE);
 CHECK(sf_nsc_seek(&client,8,(uint32_t)duplicate,1)==1);
 CHECK(sf_nsc_read(&client,7,(uint32_t)h,out,1,0,0)==1&&out[0]==1);
 CHECK(sf_nsc_open(&client,7,"/","/missing",OPEN_READ,0,0)==SF_NOT_FOUND&&client.ready);
 CHECK(!sf_nsc_reap(&client,8));CHECK(sf_nsc_close(&client,8,(uint32_t)duplicate)==SF_STALE_HANDLE);
 CHECK(!sf_nsc_close(&client,7,(uint32_t)h));
 int root=sf_nsc_open(&client,11,"/","/tmp",OPEN_READ|OPEN_WRITE|OPEN_DIRECTORY,0,0);CHECK(root>0);
 h=sf_nsc_openat(&client,11,(uint32_t)root,"file",OPEN_READ,0,0);CHECK(h>0);
 CHECK(sf_nsc_enumerate(&client,11,(uint32_t)root,(char*)out,256)==5&&!memcmp(out,"file\n",5));
 CHECK(sf_nsc_remove(&client,11,(uint32_t)root,"file",0,0,0)==SF_BUSY);
 CHECK(!sf_nsc_close(&client,11,(uint32_t)h));CHECK(!sf_nsc_remove(&client,11,(uint32_t)root,"file",0,0,0));
 CHECK(!sf_nsc_close(&client,11,(uint32_t)root));
 const SfMountEntry mount[]={{"/disk",NODE_DIR,1,0,0},{"/disk/file",NODE_DISK,1,7,4}};
 CHECK(!sf_nsc_mount_publish(&client,mount,2,1));h=sf_nsc_open(&client,12,"/","/disk/file",OPEN_READ,0,0);CHECK(h>0&&leases==1);
 duplicate=sf_nsc_dup(&client,12,(uint32_t)h);CHECK(duplicate>0&&leases==1);
 CHECK(sf_nsc_read(&client,12,(uint32_t)h,out,4,0,0)==4&&!memcmp(out,"disk",4));
 alive=0;CHECK(sf_nsc_read(&client,12,(uint32_t)h,out,1,UINT64_MAX,1)==SF_DEVICE_GONE&&client.ready);alive=1;
 CHECK(sf_nsc_unmount(&client,1)==SF_BUSY);CHECK(!sf_nsc_close(&client,12,(uint32_t)h)&&leases==1);
 CHECK(!sf_nsc_close(&client,12,(uint32_t)duplicate)&&leases==0);CHECK(!sf_nsc_unmount(&client,1));
 CHECK(!sf_nsc_project(&client,"/tmp/live",9));h=sf_nsc_open(&client,13,"/","/tmp/live",OPEN_READ,0,0);CHECK(h>0);
 CHECK(sf_nsc_read(&client,13,(uint32_t)h,out,256,0,0)==5&&!memcmp(out,"live\n",5));
 CHECK(sf_nsc_seek(&client,13,(uint32_t)h,UINT32_MAX)==UINT32_MAX);
 CHECK(!sf_nsc_close(&client,13,(uint32_t)h));
 /* Reject malformed authority/envelopes before touching the actual namespace. */
 for(unsigned bad=0;bad<9;bad++){
  SfMessage m;request(&m,OP_NS_UNMOUNT);m.mode=1;uint32_t caller=1;
  switch(bad){case 0:caller=2;m.h.flags=2;break;case 1:m.h.flags=2;break;case 2:m.h.size=39;break;
   case 3:m.h.protocol_major=2;break;case 4:m.h.protocol_minor=1;break;case 5:m.h.payload_bytes--;
   break;case 6:m.h.cap_count=1;break;case 7:m.status=1;break;case 8:m.length=sizeof(m.data)+1;break;}
  before=*server.ns;CHECK(sf_nsr_dispatch(&server,caller,&m)<0);CHECK(!memcmp(&before,server.ns,sizeof(before))&&m.length==0);
 }
 /* Zero is never a real actor. Reject it before allocating a description,
  * and accept a meaningful send deadline rather than treating it as padding. */
 SfMessage m;request(&m,OP_NS_OPEN);m.mode=OPEN_READ;m.length=23;
 memcpy(m.data+16,"/\0/tmp\0",7);before=*server.ns;
 CHECK(sf_nsr_dispatch(&server,1,&m)==SF_INVALID&&!memcmp(&before,server.ns,sizeof(before)));
 request(&m,OP_NS_BACKENDS);m.mode=server.mask;m.h.deadline_ns=UINT64_MAX;
 CHECK(!sf_nsr_dispatch(&server,1,&m));
 /* A wrong stream token cannot cancel a different active atomic publication. */
 request(&m,OP_NS_MOUNT_BEGIN);m.mode=2;m.handle=4;CHECK(!sf_nsr_dispatch(&server,1,&m));uint64_t token=m.offset;
 entry(&m,token,0,"/atomic",NODE_DIR);before=*server.ns;CHECK(!sf_nsr_dispatch(&server,1,&m));CHECK(!memcmp(&before,server.ns,sizeof(before)));
 request(&m,OP_NS_MOUNT_ABORT);m.offset=token+1;CHECK(sf_nsr_dispatch(&server,1,&m)==SF_STALE_HANDLE&&server.count==2);
 entry(&m,token,1,"/atomic/child",NODE_DIR);CHECK(!sf_nsr_dispatch(&server,1,&m));
 request(&m,OP_NS_MOUNT_COMMIT);m.offset=token;CHECK(!sf_nsr_dispatch(&server,1,&m));
 CHECK(!sf_nsc_unmount(&client,4));
 /* The actual server can reuse its integer after reinitialization. The
  * client's incarnation/generation must still reject the previous value. */
 uint32_t old=0;
 for(unsigned iteration=0;iteration<100;iteration++){
  CHECK(!boot());CHECK(!sf_nsc_create(&client,"/file",NODE_FILE,0,0,0,0644));
  h=sf_nsc_open(&client,42,"/","/file",OPEN_READ|OPEN_WRITE,0,0);CHECK(h>0);
  if(old){calls=exchanges;CHECK(sf_nsc_read(&client,42,old,out,1,0,0)==SF_STALE_HANDLE&&calls==exchanges);}
  old=(uint32_t)h;
 }
 for(unsigned mutation=1;mutation<=19;mutation++){
  CHECK(!boot());CHECK(!sf_nsc_create(&client,"/file",NODE_FILE,0,0,0,0644));
  h=sf_nsc_open(&client,42,"/","/file",OPEN_READ,0,0);CHECK(h>0);
  memset(out,0xa5,sizeof(out));unsigned previous=losses;tamper=mutation;
  int64_t result=sf_nsc_read(&client,42,(uint32_t)h,out,1,0,0);
  CHECK(result==(mutation==19?SF_TIMEOUT:SF_BAD_FORMAT));CHECK(!client.ready&&losses==previous+1);
  for(unsigned i=0;i<sizeof(out);i++)CHECK(out[i]==0xa5);
 }
 CHECK(!boot());calls=exchanges;client.sequence=UINT64_MAX;
 CHECK(sf_nsc_mount_check(&client,"/disk")==SF_NO_SPACE&&exchanges==calls);
 printf("Namespace control RPC tests passed: %u assertions\n",checks);return 0;
}
