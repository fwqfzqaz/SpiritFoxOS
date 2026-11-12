#include <stdio.h>
#include "namespace_runtime.h"
#include "namespace_client.h"
static SfReadFs fs;
static SfNsClient client;
static unsigned checks,leases,live_calls,local_reads,note_reads,bad_sender,bad_close;
static int live=1;
static unsigned large_reply,bad_length,writes;
static uint8_t payload[SF_MSG_BYTES];
#define CHECK(x) do{checks++;if(!(x)){printf("NS RUNTIME FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
/* Inject only disk bytes, not namespace behavior or packet correlation. The
 * actual SFRFS parsers stay covered by their separate integration suite. */
long sf_readfs_read(SfReadFs*f,unsigned index,uint64_t offset,void*out,size_t bytes){
 local_reads++;if(f!=&fs||index)return SF_INVALID;if(offset>=4)return 0;
 if(bytes>4-offset)bytes=(size_t)(4-offset);memcpy(out,&"disk"[offset],bytes);return(long)bytes;
}
static int64_t root_reply(void*context,SfMessage*m){
 (void)context;unsigned wanted=m->length;m->length=0;m->status=0;
 switch(m->h.opcode){
 case OP_NS_CB_STORAGE_LIVE:live_calls++;if(!live)m->status=SF_DEVICE_GONE;break;
 case OP_NS_CB_MOUNT_OPEN:if(m->handle!=1)m->status=SF_STALE_HANDLE;else leases++;break;
 case OP_NS_CB_MOUNT_CLOSE:if(bad_close)m->status=SF_BUSY;else if(leases)leases--;break;
 case OP_NS_CB_READ:
 case OP_NS_CB_PROJECTION:
  if(m->h.opcode==OP_NS_CB_READ){note_reads++;if(m->mode!=UINT32_MAX){m->status=SF_INVALID;break;}}
  if(large_reply){memcpy(m->data,payload,wanted);m->length=wanted+bad_length;m->status=m->length;}
  else if(wanted){m->data[0]='n';m->length=1;m->status=1;}break;
 case OP_NS_CB_WRITE:writes++;m->status=memcmp(m->data,payload,wanted)?SF_BAD_FORMAT:(int64_t)wanted;break;
 case OP_NS_CB_CONSOLE:m->status=large_reply&&wanted>512?SF_DENIED:(int64_t)wanted;break;
 default:m->status=SF_INVALID;break;
 }
 m->h.flags=1;m->h.deadline_ns=0;return bad_sender?2:1;
}
static int64_t exchange(void*context,SfMessage*m){
 (void)context;m->h.flags=1;sf_nsv_dispatch(1,m);if(sf_nsv_faulted())return SF_DEVICE_GONE;
 m->h.flags=77;m->h.deadline_ns=0;return 77;
}
int main(void){
 fs.mounted=1;fs.capacity=8192;fs.count=1;memcpy(fs.files[0].path,"/etc/file",10);fs.files[0].offset=4096;fs.files[0].bytes=4;
 sf_nsv_init(&fs,root_reply,0);CHECK(!sf_nsv_ready());CHECK(!sf_nsc_connect(&client,77,exchange,0,0,SF_NS_B_ALL));
 CHECK(sf_nsv_ready());CHECK(!sf_nsc_create(&client,"/ram",NODE_FILE,0,0,0,0644));
 int ram=sf_nsc_open(&client,42,"/","/ram",OPEN_READ|OPEN_WRITE,0,0);CHECK(ram>0&&sf_nsc_write(&client,42,(uint32_t)ram,"kept",4)==4);
 CHECK(sf_nsv_publish_disk(4099)==SF_BAD_FORMAT);CHECK(sf_nsc_open(&client,42,"/","/disk/etc/file",OPEN_READ,0,0)==SF_NOT_FOUND);
 CHECK(!sf_nsv_publish_disk(4100));int disk=sf_nsc_open(&client,42,"/","/disk/etc/file",OPEN_READ,0,0);CHECK(disk>0&&leases==1);
 char out[8]={0};CHECK(sf_nsc_read(&client,42,(uint32_t)disk,out,4,0,0)==4&&!memcmp(out,"disk",4));
 CHECK(live_calls==1&&local_reads==1&&!note_reads);live=0;out[0]='x';
 CHECK(sf_nsc_read(&client,42,(uint32_t)disk,out,1,UINT64_MAX,1)==SF_DEVICE_GONE&&out[0]=='x');
 CHECK(live_calls==2&&local_reads==1&&!sf_nsv_faulted()&&client.ready);
 CHECK(sf_nsc_seek(&client,42,(uint32_t)ram,0)==0&&sf_nsc_read(&client,42,(uint32_t)ram,out,4,0,0)==4&&!memcmp(out,"kept",4));
 CHECK(sf_nsc_unmount(&client,1)==SF_BUSY);CHECK(!sf_nsc_close(&client,42,(uint32_t)disk)&&leases==0);
 CHECK(!sf_nsc_unmount(&client,1));fs.mounted=0;
 CHECK(sf_nsc_seek(&client,42,(uint32_t)ram,0)==0&&sf_nsc_read(&client,42,(uint32_t)ram,out,4,0,0)==4&&!memcmp(out,"kept",4));
 const SfMountEntry notes[]={{"/persist",NODE_DIR,1,0,0},{"/persist/notes",NODE_DISK,0,UINT32_MAX,SF_MSG_BYTES}};
 CHECK(!sf_nsc_mount_publish(&client,notes,2,1));int note=sf_nsc_open(&client,42,"/","/persist/notes",OPEN_READ|OPEN_WRITE,0,0);CHECK(note>0);
 CHECK(sf_nsc_read(&client,42,(uint32_t)note,out,1,0,0)==1&&out[0]=='n'&&note_reads==1&&local_reads==1);
 /* Exercise real control/client/runtime callbacks beyond the old 256-byte
  * limit, including a complete payload reply and an independent guard. */
 for(unsigned i=0;i<sizeof(payload);i++)payload[i]=(uint8_t)(i*13u+7u);
 CHECK(!sf_nsc_create(&client,"/console",NODE_CONSOLE,0,0,0,0666));
 CHECK(!sf_nsc_project(&client,"/projection",9));
 int console=sf_nsc_open(&client,42,"/","/console",OPEN_WRITE,0,0);
 int projection=sf_nsc_open(&client,42,"/","/projection",OPEN_READ,0,0);CHECK(console>0&&projection>0);
 uint8_t guarded[SF_MSG_BYTES+2];const unsigned sizes[]={257,512,SF_MSG_BYTES};large_reply=1;
 for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++){
  unsigned n=sizes[i];memset(guarded,0xa5,sizeof(guarded));
  CHECK(sf_nsc_read(&client,42,(uint32_t)note,guarded+1,n,0,1)==n&&!memcmp(guarded+1,payload,n)&&guarded[0]==0xa5&&guarded[n+1]==0xa5);
  CHECK(sf_nsc_seek(&client,42,(uint32_t)note,0)==0&&sf_nsc_write(&client,42,(uint32_t)note,payload,n)==n);
  CHECK(sf_nsc_write(&client,42,(uint32_t)console,payload,n)==(n>512?SF_DENIED:(int64_t)n)&&!sf_nsv_faulted());
  memset(guarded,0xa5,sizeof(guarded));
  CHECK(sf_nsc_read(&client,42,(uint32_t)projection,guarded+1,n,0,1)==n&&!memcmp(guarded+1,payload,n)&&guarded[0]==0xa5&&guarded[n+1]==0xa5);
 }
 CHECK(writes==3&&client.ready&&!sf_nsv_faulted());unsigned calls=note_reads;
 CHECK(sf_nsc_read(&client,42,(uint32_t)note,guarded+1,SF_MSG_BYTES+1,0,1)==SF_INVALID&&note_reads==calls);
 CHECK(!sf_nsc_close(&client,42,(uint32_t)console)&&!sf_nsc_close(&client,42,(uint32_t)projection));large_reply=0;
 CHECK(!sf_nsc_close(&client,42,(uint32_t)note));CHECK(!sf_nsc_unmount(&client,1));
 fs.mounted=1;live=1;CHECK(!sf_nsv_publish_disk(4100));disk=sf_nsc_open(&client,42,"/","/disk/etc/file",OPEN_READ,0,0);CHECK(disk>0);
 bad_close=1;CHECK(sf_nsc_close(&client,42,(uint32_t)disk)==SF_DEVICE_GONE&&sf_nsv_faulted()&&!client.ready);
 /* A fresh service with a forged Root callback sender fails closed too. */
 bad_close=0;leases=0;sf_nsv_init(&fs,root_reply,0);CHECK(!sf_nsc_connect(&client,77,exchange,0,0,SF_NS_B_ALL));CHECK(!sf_nsv_publish_disk(4100));
 bad_sender=1;CHECK(sf_nsc_open(&client,42,"/","/disk/etc/file",OPEN_READ,0,0)==SF_DEVICE_GONE&&sf_nsv_faulted());
 bad_sender=0;leases=0;sf_nsv_init(&fs,root_reply,0);CHECK(!sf_nsc_connect(&client,77,exchange,0,0,SF_NS_B_ALL));
 CHECK(!sf_nsc_mount_publish(&client,notes,2,1));note=sf_nsc_open(&client,42,"/","/persist/notes",OPEN_READ,0,0);CHECK(note>0);
 large_reply=1;bad_length=SF_MSG_BYTES;memset(guarded,0xa5,sizeof(guarded));
 CHECK(sf_nsc_read(&client,42,(uint32_t)note,guarded+1,1,0,1)==SF_DEVICE_GONE&&sf_nsv_faulted()&&!client.ready);
 CHECK(guarded[0]==0xa5&&guarded[1]==0xa5&&guarded[sizeof(guarded)-1]==0xa5);
 printf("Namespace Fs-runtime tests passed: %u assertions\n",checks);return 0;
}
