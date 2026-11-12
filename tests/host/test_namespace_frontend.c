#include <stdio.h>
#include "namespace_frontend.h"
#include "namespace_protocol.h"
static unsigned checks,reads,writes,opens,closes,losses,exchanges;
static uint64_t last_offset;
static int live=1,fail_close;
#define CHECK(x) do{checks++;if(!(x)){printf("NS CALLBACK FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int sf_storage_namespace_start(void){return 77;}
int64_t sf_storage_namespace_exchange(void*context,SfMessage*m){
 (void)context;exchanges++;m->h.flags=77;m->h.deadline_ns=0;m->status=0;m->length=0;return 77;
}
int sf_storage_namespace_disk_live(void){return live?0:SF_DEVICE_GONE;}
void sf_init_namespace_lost(void){losses++;}
static long console(const void*data,size_t n){(void)data;return(long)n;}
static long notes(unsigned index,uint64_t offset,void*out,size_t bytes){
 reads++;if(index!=UINT32_MAX)return SF_INVALID;if(!live)return SF_DEVICE_GONE;
 last_offset=offset;if(offset>=4)return 0;if(bytes>4-offset)bytes=(size_t)(4-offset);
 memcpy(out,&"note"[offset],bytes);return(long)bytes;
}
static long write_notes(unsigned index,uint64_t offset,const void*data,size_t bytes){
 (void)data;writes++;last_offset=offset;return index==UINT32_MAX?(long)bytes:SF_INVALID;
}
static int truncate_notes(unsigned index,uint64_t bytes){last_offset=bytes;return index==UINT32_MAX?SF_NO_SPACE:SF_INVALID;}
static int mount_open(unsigned tag){opens++;return tag==1?0:SF_STALE_HANDLE;}
static void mount_close(unsigned tag){if(tag==1)closes++;if(fail_close)sf_ns_peer_lost();}
static long project(unsigned id,uint64_t offset,void*out,size_t bytes){
 if(id!=9)return SF_NOT_FOUND;if(offset>=4)return 0;if(bytes>4-offset)bytes=(size_t)(4-offset);
 memcpy(out,&"live"[offset],bytes);return(long)bytes;
}
static void request(SfMessage*m,unsigned op){memset(m,0,sizeof(*m));m->h.size=40;m->h.protocol_major=1;m->h.flags=77;m->h.payload_bytes=sizeof(*m)-40;m->h.opcode=op;m->h.request_id=123;}
int main(void){
 SfNamespace*ns=sf_ns_default();sf_ns_init(ns,console);CHECK(sf_ns_client_healthy()&&exchanges==1);
 CHECK(!sf_ns_external(ns,notes,write_notes,truncate_notes));sf_ns_mount_hooks(ns,mount_open,mount_close);sf_ns_projection_backend(ns,project);
 SfMessage m;request(&m,OP_NS_CB_CONSOLE);m.length=4;memcpy(m.data,"text",4);
 CHECK(sf_ns_client_callback(77,&m)==4&&m.status==4&&m.length==0);
 request(&m,OP_NS_CB_READ);m.mode=UINT32_MAX;m.length=256;
 CHECK(sf_ns_client_callback(77,&m)==4&&m.length==4&&!memcmp(m.data,"note",4));CHECK(reads==1);
 request(&m,OP_NS_CB_READ);m.mode=7;m.length=256;
 CHECK(sf_ns_client_callback(77,&m)==SF_DENIED&&m.length==0&&reads==1);
 request(&m,OP_NS_CB_READ);m.mode=UINT32_MAX;m.length=1;m.offset=UINT64_MAX;live=0;
 CHECK(sf_ns_client_callback(77,&m)==SF_DEVICE_GONE&&sf_ns_client_healthy());live=1;
 request(&m,OP_NS_CB_WRITE);m.mode=UINT32_MAX;m.length=256;m.offset=UINT64_MAX;
 CHECK(sf_ns_client_callback(77,&m)==256&&m.length==0&&writes==1&&last_offset==UINT64_MAX);
 request(&m,OP_NS_CB_WRITE);m.mode=7;m.length=256;
 CHECK(sf_ns_client_callback(77,&m)==SF_DENIED&&writes==1);
 request(&m,OP_NS_CB_TRUNCATE);m.mode=UINT32_MAX;m.offset=UINT64_MAX;
 CHECK(sf_ns_client_callback(77,&m)==SF_NO_SPACE&&last_offset==UINT64_MAX);
 request(&m,OP_NS_CB_MOUNT_OPEN);m.handle=1;CHECK(!sf_ns_client_callback(77,&m)&&opens==1);
 request(&m,OP_NS_CB_MOUNT_CLOSE);m.handle=1;CHECK(!sf_ns_client_callback(77,&m)&&closes==1);
 request(&m,OP_NS_CB_MOUNT_OPEN);m.handle=1ull<<32;
 CHECK(sf_ns_client_callback(77,&m)==SF_INVALID&&opens==1);
 request(&m,OP_NS_CB_PROJECTION);m.mode=9;m.offset=1;m.length=256;
 CHECK(sf_ns_client_callback(77,&m)==3&&m.length==3&&!memcmp(m.data,"ive",3));
 request(&m,OP_NS_CB_STORAGE_LIVE);CHECK(!sf_ns_client_callback(77,&m));live=0;
 request(&m,OP_NS_CB_STORAGE_LIVE);CHECK(sf_ns_client_callback(77,&m)==SF_DEVICE_GONE);live=1;
 for(unsigned bad=0;bad<10;bad++){
  request(&m,OP_NS_CB_READ);m.mode=UINT32_MAX;m.length=1;uint32_t sender=77;
  switch(bad){case 0:sender=78;break;case 1:m.h.flags=1;break;case 2:m.h.size--;break;
   case 3:m.h.protocol_major++;break;case 4:m.h.protocol_minor++;break;case 5:m.h.payload_bytes--;
   break;case 6:m.h.cap_count=1;break;case 7:m.h.request_id=0;break;case 8:m.status=1;break;case 9:m.length=sizeof(m.data)+1;break;}
  unsigned previous=reads;CHECK(sf_ns_client_callback(sender,&m)<0&&m.length==0&&reads==previous);
 }
 fail_close=1;request(&m,OP_NS_CB_MOUNT_CLOSE);m.handle=1;CHECK(sf_ns_client_callback(77,&m)==SF_DEVICE_GONE&&losses==1&&!sf_ns_client_healthy());
 sf_ns_peer_lost();CHECK(!sf_ns_client_healthy()&&losses==1);sf_ns_peer_lost();CHECK(losses==1);
 request(&m,OP_NS_CB_READ);m.mode=UINT32_MAX;m.length=1;
 CHECK(sf_ns_client_callback(77,&m)==SF_DENIED&&losses==1);
 printf("Namespace root-callback tests passed: %u assertions\n",checks);return 0;
}
