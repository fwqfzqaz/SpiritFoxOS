#include <stdio.h>
#include "namespace_frontend.h"
#include "namespace_protocol.h"
static unsigned checks,reads,writes,opens,closes,losses,exchanges,backend_exchanges;
static unsigned consoles,truncates,projections,replacement_calls;
static uint64_t last_offset;
static int live=1,fail_close,fenced;
int sf_persist_fenced(void){return fenced;}
#define CHECK(x) do{checks++;if(!(x)){printf("NS CALLBACK FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int sf_storage_namespace_start(void){return 77;}
int64_t sf_storage_namespace_exchange(void*context,SfMessage*m){
 (void)context;exchanges++;if(m->h.opcode==OP_NS_BACKENDS)backend_exchanges++;
 if(m->h.opcode==OP_NS_MOUNT_BEGIN)m->offset=73;
 m->h.flags=77;m->h.deadline_ns=0;m->status=0;m->length=0;return 77;
}
int sf_storage_namespace_disk_live(void){return live?0:SF_DEVICE_GONE;}
void sf_init_namespace_lost(void){losses++;}
static long console(const void*data,size_t n){(void)data;consoles++;return(long)n;}
static long notes(unsigned index,uint64_t offset,void*out,size_t bytes){
 reads++;if(index!=UINT32_MAX)return SF_INVALID;if(!live)return SF_DEVICE_GONE;
 last_offset=offset;if(offset>=4)return 0;if(bytes>4-offset)bytes=(size_t)(4-offset);
 memcpy(out,&"note"[offset],bytes);return(long)bytes;
}
static long write_notes(unsigned index,uint64_t offset,const void*data,size_t bytes){
 (void)data;writes++;last_offset=offset;return index==UINT32_MAX?(long)bytes:SF_INVALID;
}
static int truncate_notes(unsigned index,uint64_t bytes){truncates++;last_offset=bytes;return index==UINT32_MAX?SF_NO_SPACE:SF_INVALID;}
static int mount_open(unsigned tag){opens++;return tag==1?0:SF_STALE_HANDLE;}
static void mount_close(unsigned tag){if(tag==1)closes++;if(fail_close)sf_ns_peer_lost();}
static long project(unsigned id,uint64_t offset,void*out,size_t bytes){
 projections++;
 if(id!=9)return SF_NOT_FOUND;if(offset>=4)return 0;if(bytes>4-offset)bytes=(size_t)(4-offset);
 memcpy(out,&"live"[offset],bytes);return(long)bytes;
}
static void request(SfMessage*m,unsigned op){memset(m,0,sizeof(*m));m->h.size=40;m->h.protocol_major=1;m->h.flags=77;m->h.payload_bytes=sizeof(*m)-40;m->h.opcode=op;m->h.request_id=123;}
static long replacement_console(const void*data,size_t bytes){(void)data;(void)bytes;replacement_calls++;return SF_DENIED;}
static long replacement_read(unsigned index,uint64_t offset,void*out,size_t bytes){
 replacement_calls++;if(index!=UINT32_MAX)return SF_INVALID;if(offset>=4)return 0;
 if(bytes>4-offset)bytes=(size_t)(4-offset);memcpy(out,&"swap"[offset],bytes);return(long)bytes;
}
static long replacement_write(unsigned index,uint64_t offset,const void*data,size_t bytes){
 (void)offset;(void)data;replacement_calls++;return index==UINT32_MAX?(long)bytes:SF_INVALID;
}
static int replacement_truncate(unsigned index,uint64_t bytes){(void)bytes;replacement_calls++;return index==UINT32_MAX?SF_BUSY:SF_INVALID;}
static int replacement_open(unsigned tag){(void)tag;replacement_calls++;return SF_DENIED;}
static void replacement_close(unsigned tag){(void)tag;replacement_calls++;}
static long replacement_project(unsigned id,uint64_t offset,void*out,size_t bytes){
 replacement_calls++;if(id!=9)return SF_NOT_FOUND;if(offset>=4)return 0;
 if(bytes>4-offset)bytes=(size_t)(4-offset);memcpy(out,&"swap"[offset],bytes);return(long)bytes;
}
static int original_callbacks(void){
 unsigned old_console=consoles,old_read=reads,old_write=writes,old_truncate=truncates;
 unsigned old_open=opens,old_close=closes,old_projection=projections,old_replacements=replacement_calls;
 SfMessage m;request(&m,OP_NS_CB_CONSOLE);m.length=4;memcpy(m.data,"text",4);
 CHECK(sf_ns_client_callback(77,&m)==4&&consoles==old_console+1);
 request(&m,OP_NS_CB_READ);m.mode=UINT32_MAX;m.length=4;
 CHECK(sf_ns_client_callback(77,&m)==4&&m.length==4&&!memcmp(m.data,"note",4)&&reads==old_read+1);
 request(&m,OP_NS_CB_WRITE);m.mode=UINT32_MAX;m.offset=17;m.length=2;memcpy(m.data,"xy",2);
 CHECK(sf_ns_client_callback(77,&m)==2&&writes==old_write+1&&last_offset==17);
 request(&m,OP_NS_CB_TRUNCATE);m.mode=UINT32_MAX;m.offset=13;
 CHECK(sf_ns_client_callback(77,&m)==SF_NO_SPACE&&truncates==old_truncate+1&&last_offset==13);
 request(&m,OP_NS_CB_MOUNT_OPEN);m.handle=1;
 CHECK(!sf_ns_client_callback(77,&m)&&opens==old_open+1);
 request(&m,OP_NS_CB_MOUNT_CLOSE);m.handle=1;
 CHECK(!sf_ns_client_callback(77,&m)&&closes==old_close+1);
 request(&m,OP_NS_CB_PROJECTION);m.mode=9;m.offset=1;m.length=3;
 CHECK(sf_ns_client_callback(77,&m)==3&&m.length==3&&!memcmp(m.data,"ive",3)&&projections==old_projection+1);
 CHECK(replacement_calls==old_replacements&&sf_ns_client_healthy());return 0;
}
static int fenced_setter_tests(SfNamespace*ns){
 /* Reconnect after the original peer-loss tests, then install observable
  * callbacks through the real separately-linked, opaque frontend. */
 fail_close=0;live=1;unsigned old=exchanges,old_backends=backend_exchanges;
 sf_ns_init(ns,console);CHECK(sf_ns_client_healthy()&&exchanges==old+1&&backend_exchanges==old_backends);
 CHECK(!sf_ns_external(ns,notes,write_notes,truncate_notes));
 sf_ns_mount_hooks(ns,mount_open,mount_close);sf_ns_projection_backend(ns,project);
 CHECK(exchanges==old+4&&backend_exchanges==old_backends+3);
 CHECK(!original_callbacks());
 const SfMountEntry entries[]={{"/guard",NODE_DIR,1,0,0}};
 old=exchanges;old_backends=backend_exchanges;unsigned replacements_before=replacement_calls;
 fenced=1;
 for(unsigned attempt=0;attempt<14;attempt++){
  switch(attempt){
  case 0:sf_ns_mount_hooks(ns,0,0);break;
  case 1:sf_ns_mount_hooks(ns,replacement_open,replacement_close);break;
  case 2:CHECK(sf_ns_external(ns,0,0,0)==notes);break;
  case 3:CHECK(sf_ns_external(ns,replacement_read,replacement_write,replacement_truncate)==notes);break;
  case 4:sf_ns_projection_backend(ns,0);break;
  case 5:sf_ns_projection_backend(ns,replacement_project);break;
  case 6:sf_ns_init(ns,replacement_console);break;
  case 7:CHECK(sf_ns_mount_publish(ns,entries,1,1)==SF_DRAINING);break;
  case 8:CHECK(sf_ns_unmount(ns,1)==SF_DRAINING);break;
  case 9:CHECK(sf_ns_assign(ns,"/",1,1,0777)==SF_DRAINING);break;
  case 10:CHECK(sf_ns_project(ns,"/new-projection",17)==SF_DRAINING);break;
  case 11:CHECK(sf_ns_protect(ns,"/")==SF_DRAINING);break;
  case 12:CHECK(sf_ns_create_owned(ns,"/disk-alias",NODE_DISK,0,0,0,0600)==SF_DRAINING);break;
  case 13:CHECK(sf_ns_create(ns,"/projection-alias",NODE_PROJECTION,0)==SF_DRAINING);break;
  }
  CHECK(exchanges==old&&backend_exchanges==old_backends&&sf_ns_client_healthy());
  CHECK(!original_callbacks());
  CHECK(replacement_calls==replacements_before);
 }
 fenced=0;
 /* Clearing and replacement must become effective again after cancellation;
  * each setter advertises exactly one backend change on the actual frontend. */
 old=exchanges;old_backends=backend_exchanges;sf_ns_mount_hooks(ns,0,0);
 CHECK(exchanges==old+1&&backend_exchanges==old_backends+1);
 SfMessage m;request(&m,OP_NS_CB_MOUNT_OPEN);m.handle=1;CHECK(sf_ns_client_callback(77,&m)==SF_DEVICE_GONE);
 request(&m,OP_NS_CB_MOUNT_CLOSE);m.handle=1;CHECK(sf_ns_client_callback(77,&m)==SF_DEVICE_GONE&&sf_ns_client_healthy());
 sf_ns_mount_hooks(ns,replacement_open,replacement_close);
 CHECK(exchanges==old+2&&backend_exchanges==old_backends+2);
 request(&m,OP_NS_CB_MOUNT_OPEN);m.handle=1;CHECK(sf_ns_client_callback(77,&m)==SF_DENIED);
 request(&m,OP_NS_CB_MOUNT_CLOSE);m.handle=1;CHECK(!sf_ns_client_callback(77,&m)&&replacement_calls==replacements_before+2);
 sf_ns_mount_hooks(ns,mount_open,mount_close);CHECK(exchanges==old+3&&backend_exchanges==old_backends+3);
 old=exchanges;old_backends=backend_exchanges;
 CHECK(sf_ns_external(ns,0,0,0)==notes&&exchanges==old+1&&backend_exchanges==old_backends+1);
 request(&m,OP_NS_CB_READ);m.mode=UINT32_MAX;m.length=4;CHECK(sf_ns_client_callback(77,&m)==SF_DEVICE_GONE);
 request(&m,OP_NS_CB_WRITE);m.mode=UINT32_MAX;m.length=2;CHECK(sf_ns_client_callback(77,&m)==SF_DEVICE_GONE);
 request(&m,OP_NS_CB_TRUNCATE);m.mode=UINT32_MAX;CHECK(sf_ns_client_callback(77,&m)==SF_DEVICE_GONE);
 CHECK(!sf_ns_external(ns,replacement_read,replacement_write,replacement_truncate)&&exchanges==old+2&&backend_exchanges==old_backends+2);
 request(&m,OP_NS_CB_READ);m.mode=UINT32_MAX;m.length=4;
 CHECK(sf_ns_client_callback(77,&m)==4&&m.length==4&&!memcmp(m.data,"swap",4));
 request(&m,OP_NS_CB_WRITE);m.mode=UINT32_MAX;m.length=2;CHECK(sf_ns_client_callback(77,&m)==2);
 request(&m,OP_NS_CB_TRUNCATE);m.mode=UINT32_MAX;CHECK(sf_ns_client_callback(77,&m)==SF_BUSY&&replacement_calls==replacements_before+5);
 CHECK(sf_ns_external(ns,notes,write_notes,truncate_notes)==replacement_read&&exchanges==old+3&&backend_exchanges==old_backends+3);
 old=exchanges;old_backends=backend_exchanges;sf_ns_projection_backend(ns,0);
 CHECK(exchanges==old+1&&backend_exchanges==old_backends+1);
 request(&m,OP_NS_CB_PROJECTION);m.mode=9;m.offset=1;m.length=3;CHECK(sf_ns_client_callback(77,&m)==SF_DEVICE_GONE);
 sf_ns_projection_backend(ns,replacement_project);CHECK(exchanges==old+2&&backend_exchanges==old_backends+2);
 request(&m,OP_NS_CB_PROJECTION);m.mode=9;m.offset=1;m.length=3;
 CHECK(sf_ns_client_callback(77,&m)==3&&m.length==3&&!memcmp(m.data,"wap",3)&&replacement_calls==replacements_before+6);
 sf_ns_projection_backend(ns,project);CHECK(exchanges==old+3&&backend_exchanges==old_backends+3);
 CHECK(!original_callbacks());
 old=exchanges;old_backends=backend_exchanges;
 CHECK(!sf_ns_mount_publish(ns,entries,1,1)&&exchanges==old+3&&backend_exchanges==old_backends);
 CHECK(!sf_ns_unmount(ns,1)&&exchanges==old+4&&backend_exchanges==old_backends&&sf_ns_client_healthy());
 sf_ns_init(ns,replacement_console);CHECK(exchanges==old+5&&backend_exchanges==old_backends&&sf_ns_client_healthy());
 request(&m,OP_NS_CB_CONSOLE);m.length=4;memcpy(m.data,"text",4);
 CHECK(sf_ns_client_callback(77,&m)==SF_DENIED&&replacement_calls==replacements_before+7);
 return 0;
}
int main(void){
 SfNamespace*ns=sf_ns_default();sf_ns_init(ns,console);CHECK(sf_ns_client_healthy()&&exchanges==1);
 unsigned previous_exchanges=exchanges;fenced=1;
 sf_ns_init(ns,0);CHECK(sf_ns_client_healthy()&&exchanges==previous_exchanges);
 CHECK(sf_ns_mount_publish(ns,0,0,1)==SF_DRAINING&&exchanges==previous_exchanges&&sf_ns_client_healthy());
 CHECK(sf_ns_unmount(ns,1)==SF_DRAINING&&exchanges==previous_exchanges&&sf_ns_client_healthy());fenced=0;
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
 CHECK(!fenced_setter_tests(ns));
 printf("Namespace root-callback tests passed: %u assertions\n",checks);return 0;
}
