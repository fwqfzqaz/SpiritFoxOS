#include "libsf.h"
#include "storage.h"
#include "persistence.h"
#include "virtio.h"
#include "block_rpc.h"
#include "txn.h"
#include "namespace_frontend.h"
#include "namespace_transport.h"
#include "namespace_protocol.h"
#include "fs_format.h"
/* sf-init side of the disk filesystem split: format dispatch and file reads live
 * in sf-fs.sfmod. sf-init remains the sole receiver of virtio-blk replies and
 * answers the service's bounded OP_FS_BLOCK_READ requests through a broker. */
static SfManager*manager;
static SfNamespace*ns;
static SfModuleSpawn spawn;
static uint32_t driver_reply,driver_ep,driver_pid,granted_device;
static uint32_t fs_service_ep,fs_service_reply,fs_broker_ep,fs_broker_reply,fs_pid;
static int driver_slot=-1,fs_slot=-1,mounted,fs_format_id,driver_kind=-1;
/* 真实硬件：按 PCI class code 选择存储驱动。virtio 优先（开发/回归环境），
 * 其次 SATA AHCI，最后 NVMe。启动时逐个候选控制器尝试，直到挂载成功。 */
enum {DRV_VIRTIO,DRV_AHCI,DRV_NVME};
static uint32_t candidate_bdf[64];static int candidate_kind[64];static unsigned candidate_count,candidate_next;
static int classify(uint32_t bdf){
 uint32_t id=(uint32_t)sf_sys(SYS_PCI_READ,bdf,0,0);
 if(id==0x10011af4u||id==0x10421af4u)return DRV_VIRTIO;
 uint32_t rev=(uint32_t)sf_sys(SYS_PCI_READ,bdf,8,0);
 if((rev>>24)==1&&((rev>>16)&0xff)==6&&((rev>>8)&0xff)==1)return DRV_AHCI;
 if((rev>>24)==1&&((rev>>16)&0xff)==8&&((rev>>8)&0xff)==2)return DRV_NVME;
 return -1;
}
static void build_candidates(void){
 candidate_count=candidate_next=0;
 for(int kind=0;kind<=DRV_NVME;kind++)
  for(unsigned bdf=0;bdf<256&&candidate_count<64;bdf++)
   if(classify(bdf)==kind){candidate_kind[candidate_count]=kind;candidate_bdf[candidate_count++]=(uint32_t)bdf+1;}
}
static const char* driver_package(int kind){return kind==DRV_AHCI?"sf-ahci.sfmod":kind==DRV_NVME?"sf-nvme.sfmod":"sf-virtio-blk.sfmod";}
static const char* driver_module(int kind){return kind==DRV_AHCI?"org.spiritfox.ahci":kind==DRV_NVME?"org.spiritfox.nvme":"org.spiritfox.virtio.blk";}
static uint64_t capacity,sequence=0x100000000ull,fs_requests=0x400000000ull;
/* 与 sf-init 的包快照共用一块 128 KiB 只读装载缓冲。服务单线程处理，
 * 两条路径不会嵌套；每次借出前作废模块侧缓存状态。 */
static SfNsTransport fs_transport;
static uint32_t namespace_descriptions;
static int mount_admission,mount_driver_lease;
static uint8_t load_scratch[SF_STORAGE_SNAPSHOT_BYTES];
uint8_t* sf_storage_scratch(void){return load_scratch;}
int sf_storage_driver(uint32_t*ep,uint32_t*reply,uint32_t*pid,uint64_t*bytes,int*writable){
 if(!driver_pid||!driver_ep||!driver_reply)return SF_DEVICE_GONE;
 if(ep)*ep=driver_ep;if(reply)*reply=driver_reply;if(pid)*pid=driver_pid;
 if(bytes)*bytes=capacity;if(writable)*writable=sf_storage_writable();
 return 0;
}
static int writable,write_grant;
static int call(SfMessage*m){
 if(!driver_pid||sf_sys(SYS_TASK_STATUS,driver_pid,0,0)!=SF_TASK_RUNNING)return SF_DEVICE_GONE;
 if(sequence==UINT64_MAX)return SF_NO_SPACE;
 m->h.request_id=++sequence;SfBlockRequest request=sf_block_request(m);
 int r=sf_send(driver_ep,m);if(r){if(r>0)sf_sys(SYS_KILL,driver_pid,SF_DEVICE_GONE,0);return r>0?SF_BAD_FORMAT:r;}
 long from=sf_recv(driver_reply,m,sf_deadline(2000));if(from<0){sf_sys(SYS_KILL,driver_pid,SF_DEVICE_GONE,0);return from;}
 if(m->h.cap_count==1)sf_cap_close(m->handle);
 r=sf_block_reply(&request,m,from,driver_pid);if(r){sf_sys(SYS_KILL,driver_pid,SF_DEVICE_GONE,0);return r;}
 return(int)m->status;
}
static int sector(void*context,uint64_t lba,uint8_t out[512]){
 if(lba>=capacity/512)return SF_INVALID;
 for(unsigned half=0;half<512;half+=256){SfMessage m;sf_message(&m,OP_BLOCK_READ,0);m.offset=lba*512;m.mode=half;m.length=256;int r=call(&m);if(r)return r;if(m.length!=256)return SF_BAD_FORMAT;memcpy(out+half,m.data,256);}return 0;
}
/* Reply to one fs block request. Zero timeout means the caller already polls. */
int sf_storage_broker(unsigned timeout_ms){
 if(!fs_broker_ep||!fs_broker_reply||!fs_pid)return 0;
 if(sf_sys(SYS_TASK_STATUS,fs_pid,0,0)!=SF_TASK_RUNNING)return 0;
 SfMessage m;long from=sf_recv(fs_broker_ep,&m,sf_deadline(timeout_ms));
 if(from<0)return from==SF_TIMEOUT?0:(int)from;
 if(m.h.cap_count==1)sf_cap_close(m.handle);
 if(from!=(long)fs_pid)return SF_DENIED;
 if(m.h.opcode>=OP_NS_CB_CONSOLE&&m.h.opcode<=OP_NS_CB_STORAGE_LIVE)sf_ns_client_callback((uint32_t)from,&m);
 else if(m.h.flags!=fs_pid||m.h.size!=40||m.h.protocol_major!=1||m.h.protocol_minor||m.h.payload_bytes!=sizeof(m)-40||m.h.cap_count||
         m.h.opcode!=OP_FS_BLOCK_READ||m.length!=256||(m.mode!=0&&m.mode!=256)||(m.offset&511)){m.status=SF_INVALID;m.length=0;}
 else{
  uint8_t data[512];int r=sector(0,m.offset/512,data);m.status=r;
  if(!r){memcpy(m.data,data+m.mode,256);m.length=256;}else m.length=0;
 }
 m.h.cap_count=0;m.h.deadline_ns=0;
 int r=sf_send(fs_broker_reply,&m);return r?r:1;
}
static int64_t transport_send(void*c,uint32_t cap,SfMessage*m){(void)c;return sf_send(cap,m);}
static int64_t transport_receive(void*c,uint32_t cap,SfMessage*m,unsigned ms){(void)c;return sf_recv(cap,m,sf_deadline(ms));}
static uint64_t transport_ticks(void*c){(void)c;return sf_ticks();}
static int transport_alive(void*c,uint32_t pid){(void)c;return sf_sys(SYS_TASK_STATUS,pid,0,0)==SF_TASK_RUNNING;}
static int transport_pump(void*c,unsigned ms){(void)c;return sf_storage_broker(ms);}
static void transport_close(void*c,uint64_t cap){(void)c;sf_cap_close(cap);}
int64_t sf_storage_namespace_exchange(void*context,SfMessage*m){
 (void)context;
 fs_transport.ops=(SfNsTransportOps){transport_send,transport_receive,transport_ticks,transport_alive,transport_pump,transport_close};
 fs_transport.service=fs_service_ep;fs_transport.reply=fs_service_reply;fs_transport.peer=fs_pid;
 return sf_nst_exchange(&fs_transport,m);
}
static int fs_status_call(SfMessage*m){
 int64_t from=sf_storage_namespace_exchange(0,m);
 if(from<0){sf_ns_peer_lost();return(int)from;}
 if(m->length>sizeof(m->data)||m->status>0||m->status<SF_VERSION_REJECTED){sf_ns_peer_lost();return SF_BAD_FORMAT;}
 return(int)m->status;
}
static int fs_call(SfMessage*m){
 if(!fs_pid||!fs_service_ep||!fs_service_reply)return SF_DEVICE_GONE;
 if(fs_requests==UINT64_MAX)return SF_NO_SPACE;
 m->h.request_id=++fs_requests;m->h.cap_count=0;return fs_status_call(m);
}
static int fs_send_cap(SfMessage*m,uint32_t cap){
 if(!fs_pid||!fs_service_ep||!fs_service_reply)return SF_DEVICE_GONE;
 if(fs_requests==UINT64_MAX)return SF_NO_SPACE;
 m->h.request_id=++fs_requests;m->h.cap_count=1;m->handle=cap;return fs_status_call(m);
}
static int create_ep(uint32_t*out){long h=sf_sys(SYS_EP_CREATE,0,0,0);if(h<0)return(int)h;*out=(uint32_t)h;return 0;}
static int fs_endpoints(void){
 int r;
 if(!fs_broker_reply&&(r=create_ep(&fs_broker_reply)))return r;
 if(!fs_broker_ep&&(r=create_ep(&fs_broker_ep)))return r;
 if(!fs_service_reply&&(r=create_ep(&fs_service_reply)))return r;
 if(!fs_service_ep&&(r=create_ep(&fs_service_ep)))return r;
 return 0;
}
static int fs_shutdown(void){
 if(fs_pid){
  if(sf_sys(SYS_TASK_STATUS,fs_pid,0,0)==SF_TASK_RUNNING)sf_sys(SYS_KILL,fs_pid,SF_DEVICE_GONE,0);
  uint64_t start=sf_ticks();
  while(sf_sys(SYS_TASK_STATUS,fs_pid,0,0)==SF_TASK_RUNNING){if(sf_ticks()-start>=100)return SF_BUSY;sf_yield();}
  fs_pid=0;
 }
 if(fs_service_ep){sf_sys(SYS_CAP_REVOKE,fs_service_ep,0,0);fs_service_ep=0;}
 if(fs_service_reply){sf_sys(SYS_CAP_REVOKE,fs_service_reply,0,0);fs_service_reply=0;}
 if(fs_broker_ep){sf_sys(SYS_CAP_REVOKE,fs_broker_ep,0,0);fs_broker_ep=0;}
 if(fs_broker_reply){sf_sys(SYS_CAP_REVOKE,fs_broker_reply,0,0);fs_broker_reply=0;}
 return 0;
}
static int fs_attach(void){
 int cap=(int)sf_cap_derive(fs_broker_ep,RIGHT_SEND|RIGHT_DELEGATE);if(cap<0)return cap;
 SfMessage m;sf_message(&m,OP_FS_ATTACH_SEND,0);m.offset=capacity;
 int r=fs_send_cap(&m,(uint32_t)cap);sf_cap_close(cap);if(r)return r;
 cap=(int)sf_cap_derive(fs_broker_reply,RIGHT_RECV|RIGHT_DELEGATE);if(cap<0)return cap;
 sf_message(&m,OP_FS_ATTACH_REPLY,0);m.offset=capacity;
 r=fs_send_cap(&m,(uint32_t)cap);sf_cap_close(cap);return r;
}
static int fs_start(void){
 if(fs_pid)return sf_sys(SYS_TASK_STATUS,fs_pid,0,0)==SF_TASK_RUNNING?0:SF_RESTART_REQUIRED;
 if(!manager||!spawn)return SF_INVALID;
 if(fs_slot<0){fs_slot=sfmod_add(manager,"org.spiritfox.fs.disk",0,0);if(fs_slot<0)return fs_slot;}
 int r=sfmod_start(manager,fs_slot);if(r)return r;
 r=fs_endpoints();if(r)goto fail;
 int pid=spawn("sf-fs.sfmod",fs_service_ep,RIGHT_RECV,0,0,fs_service_reply);
 if(pid<0){r=pid;goto fail;}
 fs_pid=(uint32_t)pid;r=fs_attach();if(r)goto fail;
 return 0;
fail:
 if(fs_shutdown())return SF_BUSY;
 sfmod_prepare(manager,fs_slot);sfmod_stop(manager,fs_slot);return r;
}
int sf_storage_namespace_start(void){int r=fs_start();return r?r:(int)fs_pid;}
uint32_t sf_storage_namespace_pid(void){return fs_pid;}
int sf_storage_namespace_disk_live(void){return mounted&&driver_pid&&sf_sys(SYS_TASK_STATUS,driver_pid,0,0)==SF_TASK_RUNNING?0:SF_DEVICE_GONE;}
int sf_storage_namespace_reset(void){
 if(fs_transport.inflight||fs_slot<0)return SF_BUSY;
 SfModule*book=&manager->modules[fs_slot];
 if(book->resources[RES_DIRTY]||book->resources[RES_OPEN]!=namespace_descriptions)return SF_BUSY;
 for(unsigned i=0;i<RES_COUNT;i++)if(i!=RES_OPEN&&book->resources[i])return SF_BUSY;
 int r=fs_shutdown();if(r)return r;
 /* Only confirmed-dead namespace descriptions are reconciled. The unrelated
  * Root load lease and dirty transaction must already have drained. */
 while(namespace_descriptions){r=sfmod_release(manager,fs_slot,RES_OPEN);if(r)return r;namespace_descriptions--;}
 if(book->state==MOD_ACTIVE){r=sfmod_prepare(manager,fs_slot);if(r)return r;}
 if(book->state==MOD_DRAINING){r=sfmod_stop(manager,fs_slot);if(r)return r;}
 if(book->state!=MOD_STOPPED)return SF_STALE_HANDLE;
 r=sfmod_unload(manager,fs_slot);if(r)return r;fs_slot=-1;
 return 0;
}
int sf_storage_namespace_crash(void){
 if(!fs_pid)return SF_NOT_FOUND;
 int r=sf_sys(SYS_KILL,fs_pid,SF_DEVICE_GONE,0);if(r)return r;
 sf_ns_peer_lost();return 0;
}
int sf_storage_sector_read(void*c,uint64_t lba,uint8_t out[512]){return sector(c,lba,out);}
int sf_storage_writable(void){return writable&&mounted&&driver_pid&&sf_sys(SYS_TASK_STATUS,driver_pid,0,0)==SF_TASK_RUNNING;}
int sf_storage_sector_write(void*c,uint64_t lba,const uint8_t data[512]){
 if(!sf_storage_writable())return SF_READ_ONLY;
 if(lba>=capacity/512)return SF_INVALID;
 for(unsigned half=0;half<512;half+=256){SfMessage m;sf_message(&m,OP_BLOCK_WRITE,0);m.offset=lba*512;m.mode=half;m.length=256;memcpy(m.data,data+half,256);int r=call(&m);if(r)return r;}return 0;
}
int sf_storage_flush(void*c){SfMessage m;sf_message(&m,OP_BLOCK_FLUSH,0);return call(&m);}
int sf_storage_write_range(uint64_t first,uint64_t end){SfMessage m;sf_message(&m,OP_BLOCK_WRITE_RANGE,0);m.offset=first;m.handle=end;return call(&m);}
int sf_storage_transaction(int begin){
 /* A failed write can kill the driver after admission. Release its ledger
  * entry even when no further storage operation is allowed. */
 if(!begin)return fs_slot>=0?sfmod_release(manager,fs_slot,RES_DIRTY):SF_STALE_HANDLE;
 if(!sf_storage_writable())return SF_READ_ONLY;
 return sfmod_acquire(manager,fs_slot,RES_DIRTY);
}
static int mount_open(unsigned tag){
 if(tag!=1||fs_slot<0)return SF_STALE_HANDLE;
 if(!mounted||!mount_admission)return SF_DRAINING;
 if(namespace_descriptions==UINT32_MAX)return SF_NO_SPACE;
 int r=sfmod_acquire(manager,fs_slot,RES_OPEN);if(!r)namespace_descriptions++;return r;
}
static void mount_close(unsigned tag){
 if(tag!=1||fs_slot<0||!namespace_descriptions||sfmod_release(manager,fs_slot,RES_OPEN)){sf_ns_peer_lost();return;}
 namespace_descriptions--;
}
static long external_read(unsigned index,uint64_t offset,void*out,size_t bytes){
 if(!mounted||!fs_pid||sf_sys(SYS_TASK_STATUS,driver_pid,0,0)!=SF_TASK_RUNNING)return SF_DEVICE_GONE;
 if(!bytes)return 0;
 long total=0;uint8_t*dst=out;
 while(bytes){
  uint32_t chunk=bytes>sizeof(((SfMessage*)0)->data)?sizeof(((SfMessage*)0)->data):(uint32_t)bytes;
  SfMessage m;sf_message(&m,OP_FS_READ,0);m.mode=index;m.offset=offset;m.length=chunk;
  int r=fs_call(&m);if(r<0)return total?total:r;
  if(!m.length)break;
  if(m.length>bytes)return total?total:SF_BAD_FORMAT;
  memcpy(dst,m.data,m.length);total+=(long)m.length;offset+=m.length;dst+=m.length;bytes-=m.length;
  if(m.length<chunk)break;
 }
 return total;
}
void sf_storage_init(SfManager*m,SfNamespace*n,SfModuleSpawn callback){manager=m;ns=n;spawn=callback;build_candidates();}
void sf_storage_namespace_bind(SfNamespace*n){sf_ns_external(n,external_read,0,0);sf_ns_mount_hooks(n,mount_open,mount_close);}
static int mount_disk_view(void){
 int r=fs_start();if(r)return r;r=fs_attach();if(r)return r;
 if(write_grant){
  uint8_t header[512],hash[32];r=sector(0,SF_TXN_HEADER,header);if(r)return r;sf_sha256(hash,header,480);
  if(capacity!=SF_TXN_SECTORS*512ull||memcmp(header,"SFTXN1\0",8)||sf_u32(header+8)!=1||sf_u32(header+12)!=SF_TXN_SECTORS||memcmp(hash,header+480,32))return SF_BAD_FORMAT;
 }
 SfMessage m;sf_message(&m,OP_FS_MOUNT,0);m.offset=write_grant?SF_TXN_HEADER*512ull:0;r=fs_call(&m);if(r)return r;
 if(m.mode<SF_FS_FORMAT_SFRFS1||m.mode>SF_FS_FORMAT_SFRFS2||m.handle>SF_READFS_MAX_ENTRIES){sf_ns_peer_lost();return SF_BAD_FORMAT;}
 fs_format_id=(int)m.mode;return 0;
}
int sf_storage_namespace_restore(void){
 if(!mounted)return 0;
 int r=mount_disk_view();if(r)return r;
 mount_admission=1;return 0;
}
static int start_candidate(unsigned index,unsigned rights){
 if(driver_pid&&sf_sys(SYS_TASK_STATUS,driver_pid,0,0)==SF_TASK_RUNNING)return SF_BUSY;
 if(index>=candidate_count)return SF_NOT_FOUND;
 int kind=candidate_kind[index];uint32_t device=candidate_bdf[index];
 if(driver_slot<0||driver_kind!=kind){
  if(driver_slot>=0){sfmod_prepare(manager,driver_slot);sfmod_stop(manager,driver_slot);}
  driver_slot=sfmod_add(manager,driver_module(kind),0,0);if(driver_slot<0)return driver_slot;driver_kind=kind;
 }
 int r=sfmod_start(manager,driver_slot);if(r)return r;driver_reply=sf_sys(SYS_EP_CREATE,0,0,0);if((int)driver_reply<0){r=(int)driver_reply;driver_reply=0;goto fail;}driver_ep=sf_sys(SYS_EP_CREATE,0,0,0);if((int)driver_ep<0){r=(int)driver_ep;goto fail;}
 int pid=spawn(driver_package(kind),driver_ep,RIGHT_RECV,device,rights,driver_reply);if(pid<0){r=pid;goto fail;}driver_pid=pid;granted_device=device;
 SfMessage message;sf_message(&message,OP_BLOCK_INFO,0);r=call(&message);if(r)goto fail;
 if(message.length!=sizeof(SfBlockInfo)){sf_log("[STORAGE] info length=");sf_number(message.length);sf_log("\n");r=SF_BAD_FORMAT;goto fail;}SfBlockInfo info;memcpy(&info,message.data,sizeof(info));if(info.sector_bytes!=512||info.readonly!=(rights?0u:1u)||!info.sectors||info.sectors>UINT64_MAX/512){sf_log("[STORAGE] info sectors=");sf_number(info.sectors);sf_log(" bytes=");sf_number(info.sector_bytes);sf_log(" ro=");sf_number(info.readonly);sf_log(" rights=");sf_number(rights);sf_log("\n");r=SF_BAD_FORMAT;goto fail;}capacity=info.sectors*512;write_grant=rights!=0;writable=0;
 sfmod_acquire(manager,driver_slot,RES_DMA);return 0;
fail:
 if(driver_pid)sf_sys(SYS_KILL,driver_pid,SF_DEVICE_GONE,0);driver_pid=0;if((int)driver_ep>0)sf_sys(SYS_CAP_REVOKE,driver_ep,0,0);driver_ep=0;if(driver_reply)sf_sys(SYS_CAP_REVOKE,driver_reply,0,0);driver_reply=0;
 sfmod_prepare(manager,driver_slot);sfmod_stop(manager,driver_slot);return r;
}
int sf_storage_start(void){
 if(!candidate_count)return SF_NOT_FOUND;
 while(candidate_next<candidate_count){
  int r=start_candidate(candidate_next,0);
  if(r==SF_READ_ONLY)r=start_candidate(candidate_next,SF_DEVICE_WRITE);
  if(!r)return 0;
  if(r==SF_BUSY)return r;
  sf_log("[STORAGE] candidate kind=");sf_number((uint64_t)candidate_kind[candidate_next]);sf_log(" code=");sf_number(r<0?-r:r);sf_log("\n");
  candidate_next++;
 }
 candidate_next=0;return SF_NOT_FOUND;
}
int sf_storage_restart_next(void){
 int r=sf_storage_stop();if(r){sf_log("[STORAGE] stop failed code=");sf_number(-r);sf_log("\n");return r;}
 if(candidate_next+1>=candidate_count){candidate_next=0;return SF_NOT_FOUND;}
 candidate_next++;return sf_storage_start();
}
int sf_storage_mount(void){
 if(mounted)return SF_BUSY;if(!driver_pid)return SF_DEVICE_GONE;
 int r=sf_ns_mount_check(ns,"/disk");if(r)return r;
 r=sfmod_acquire(manager,driver_slot,RES_LEASE);if(r)return r;mount_driver_lease=1;
 r=mount_disk_view();if(r){sfmod_release(manager,driver_slot,RES_LEASE);mount_driver_lease=0;return r;}
 writable=write_grant;mounted=1;mount_admission=1;
 sf_log("[STORAGE] format ");sf_number((uint64_t)fs_format_id);sf_log("\n");return 0;
}
int sf_storage_unmount(void){
 if(!mounted||fs_slot<0)return SF_NOT_FOUND;mount_admission=0;
 if(manager->modules[fs_slot].resources[RES_OPEN]||manager->modules[fs_slot].resources[RES_DIRTY]){mount_admission=1;return SF_BUSY;}
 int r=sf_ns_unmount(ns,1);if(r){mount_admission=1;return r;}
 SfMessage m;sf_message(&m,OP_FS_STOP,0);r=fs_call(&m);if(r){sf_ns_peer_lost();return r;}
 if(mount_driver_lease){r=sfmod_release(manager,driver_slot,RES_LEASE);if(r)return r;mount_driver_lease=0;}
 mounted=0;writable=0;sf_persist_driver_down();return 0;
}
int sf_storage_stop(void){
 if(driver_slot<0||!driver_pid)return SF_NOT_FOUND;int r=sfmod_prepare(manager,driver_slot);if(r)return r;
 if(manager->modules[driver_slot].resources[RES_LEASE]){sfmod_cancel(manager,driver_slot);return SF_DEPENDENCY;}
 for(unsigned i=0;i<RES_COUNT;i++)if(i!=RES_DMA&&manager->modules[driver_slot].resources[i]){sfmod_cancel(manager,driver_slot);return SF_BUSY;}
 if(sf_sys(SYS_TASK_STATUS,driver_pid,0,0)==SF_TASK_RUNNING){SfMessage message;sf_message(&message,OP_BLOCK_STOP,0);r=call(&message);}else r=sf_sys(SYS_DEVICE_RELEASE,driver_pid,0,0);
 /* A dead task's slot may already belong to a new PID. Core 0 completed
  * device cleanup before recycling that slot; its quarantine still gates grants. */
 if(r==SF_NOT_FOUND)r=0;
 if(r){manager->modules[driver_slot].state=MOD_QUARANTINED;return r;}
 sfmod_release(manager,driver_slot,RES_DMA);r=sfmod_stop(manager,driver_slot);if(r)return r;sf_sys(SYS_KILL,driver_pid,0,0);sf_sys(SYS_CAP_REVOKE,driver_ep,0,0);sf_sys(SYS_CAP_REVOKE,driver_reply,0,0);driver_pid=driver_ep=driver_reply=0;mounted=0;writable=0;sf_persist_driver_down();return 0;
}
long sf_storage_load(const char*path,void*out,size_t capacity_bytes){
 if(strlen(path)<7||memcmp(path,"/disk/",6))return SF_INVALID;if(fs_slot<0||sf_storage_namespace_disk_live())return SF_DEVICE_GONE;
 int r=sfmod_acquire(manager,fs_slot,RES_OPEN);if(r)return r;
 SfMessage m;sf_message(&m,OP_FS_FIND,0);size_t n=strlen(path+5)+1;if(n>sizeof(m.data)){sfmod_release(manager,fs_slot,RES_OPEN);return SF_INVALID;}
 memcpy(m.data,path+5,n);m.length=(uint32_t)n;r=fs_call(&m);
 if(r<0){sfmod_release(manager,fs_slot,RES_OPEN);return r;}
 uint64_t bytes=m.handle;if(bytes>capacity_bytes){sfmod_release(manager,fs_slot,RES_OPEN);return SF_NO_SPACE;}
 long total=0;uint64_t offset=0;uint32_t index=(uint32_t)m.offset;
 while(offset<bytes){
  uint32_t chunk=(uint32_t)(bytes-offset);if(chunk>sizeof(m.data))chunk=sizeof(m.data);
  SfMessage q;sf_message(&q,OP_FS_READ,0);q.mode=index;q.offset=offset;q.length=chunk;
  r=fs_call(&q);if(r<0){sfmod_release(manager,fs_slot,RES_OPEN);return r;}
  if(!q.length||q.length>chunk){sfmod_release(manager,fs_slot,RES_OPEN);return SF_BAD_FORMAT;}
  memcpy((uint8_t*)out+offset,q.data,q.length);offset+=q.length;total+=(long)q.length;
 }
 sfmod_release(manager,fs_slot,RES_OPEN);return total;
}
int sf_storage_ready(void){return mounted;}
int sf_storage_crash_test(void){
 if(!sf_storage_writable())return SF_DEVICE_GONE;
 int h=sf_ns_open(ns,1,"/","/persist/notes",OPEN_READ|OPEN_WRITE);if(h<0)return h;
 char data[32];long r=sf_ns_read(ns,1,h,data,sizeof(data));if(r<0){sf_ns_close(ns,1,h);return r;}
 sf_assert(!sf_storage_transaction(1),"driver failure test owns dirty lease");
 sf_assert(!sf_sys(SYS_KILL,driver_pid,SF_BUSY,0),"driver failure uses exit code SF_BUSY");sf_persist_driver_down();
 sf_assert(!sf_storage_writable()&&sf_ns_read(ns,1,h,data,1)==SF_DEVICE_GONE&&sf_ns_write(ns,1,h,"x",1)==SF_DEVICE_GONE&&sf_ns_pread(ns,1,h,data,1,0)==SF_DEVICE_GONE&&sf_ns_pread(ns,1,h,data,1,UINT64_MAX)==SF_DEVICE_GONE,"dead driver rejects held persistent read/write and positional EOF");
 sf_ns_close(ns,1,h);sf_assert(!sf_storage_transaction(0),"driver failure still releases dirty lease");
 sf_log("STORAGE CRASH TEST PASSED\n");return 0;
}
void sf_storage_dma_probe(unsigned mode){
 SfDmaStatus before,after;int r=sf_sys(SYS_DMA_STATUS,(uint64_t)&before,sizeof(before),0);
 if(r||before.mode!=1||!driver_pid||!before.active_mapping||mode>1){sf_log("DMA PROBE REFUSED\n");return;}
 SfMessage m;sf_message(&m,OP_BLOCK_DMA_PROBE,0);m.offset=before.probe_address;m.mode=mode;r=call(&m);
 sf_sys(SYS_DMA_STATUS,(uint64_t)&after,sizeof(after),0);
 sf_assert(r<0&&after.faults>before.faults&&after.last_address==before.probe_address&&after.source==granted_device-1&&after.guard_intact&&!after.active_mapping,"IOMMU blocks actual DMA outside granted arena");
 sf_assert(sf_sys(SYS_TASK_STATUS,driver_pid,0,0)==SF_DEVICE_GONE,"faulting DMA driver terminated");sf_persist_driver_down();
 int ep=sf_sys(SYS_EP_CREATE,0,0,0);sf_assert(ep>0,"quarantine test endpoint allocated");int candidate=spawn("sf-virtio-blk.sfmod",ep,RIGHT_RECV,granted_device,0,driver_reply);sf_assert(candidate==SF_BUSY,"kernel refuses new owner of quarantined DMA device");sf_sys(SYS_CAP_REVOKE,ep,0,0);
 sf_log("DMA PROBE PASSED: guard intact, device quarantined\n");
}
void sf_storage_write_tests(void){
 uint8_t zero[512]={0};sf_assert(sf_storage_sector_write(0,0,zero)==SF_INVALID,"write grant cannot overwrite immutable disk region");
 sf_assert(sf_storage_sector_write(0,capacity/512,zero)==SF_INVALID&&sf_storage_sector_write(0,(1ull<<55)+SF_TXN_BANK,zero)==SF_INVALID,"sector write rejects capacity and wrapped LBA before multiplication");
 SfMessage m;sf_message(&m,OP_BLOCK_WRITE,0);m.offset=SF_TXN_BANK*512ull;m.mode=256;m.length=256;sf_assert(call(&m)==SF_INVALID,"write service rejects missing first fragment");
 sf_assert(!sf_storage_transaction(1),"transaction acquires dirty resource");sf_assert(sf_storage_unmount()==SF_BUSY,"dirty transaction prevents unmount and restores admission");sf_assert(!sf_storage_transaction(0),"transaction releases dirty resource");
}
void sf_storage_tests(void){
 if(!mounted||writable)return;SfMessage m;sf_message(&m,OP_BLOCK_READ,0);m.offset=capacity;m.length=256;sf_assert(call(&m)==SF_INVALID,"block read rejects capacity boundary");
 sf_message(&m,OP_BLOCK_READ,0);m.offset=1;m.length=256;sf_assert(call(&m)==SF_INVALID,"block read rejects unaligned offset");sf_message(&m,OP_BLOCK_WRITE,0);sf_assert(call(&m)==SF_READ_ONLY,"block service rejects every write");
 sf_assert(sf_storage_stop()==SF_DEPENDENCY,"mounted filesystem pins block driver");int h=sf_ns_open(ns,1,"/","/disk/etc/readme.txt",1);sf_assert(h>0&&sf_storage_unmount()==SF_BUSY,"open disk file prevents unmount and restores admission");int dup=sf_ns_dup(ns,1,h);sf_ns_close(ns,1,h);sf_assert(sf_storage_unmount()==SF_BUSY,"duplicated disk description retains mount lease");sf_ns_close(ns,1,dup);
 sf_assert(sf_ns_open(ns,1,"/","/disk/etc/readme.txt",2)==SF_READ_ONLY&&sf_ns_create(ns,"/disk/etc/new",NODE_FILE,0)==SF_READ_ONLY,"read-only mount rejects writes and creation");
 sf_assert(!sf_storage_unmount(),"unmount drains file references");sf_message(&m,OP_BLOCK_STOP,0);sf_assert(!call(&m),"driver quiesces before process teardown");
 int candidate_ep=sf_sys(SYS_EP_CREATE,0,0,0);sf_assert(candidate_ep>0,"replacement endpoint allocated");int replacement=spawn("sf-virtio-blk.sfmod",candidate_ep,RIGHT_RECV,granted_device,0,driver_reply);sf_assert(replacement==SF_BUSY,"live stopped driver cannot share DMA arena with replacement");sf_sys(SYS_CAP_REVOKE,candidate_ep,0,0);
 sf_assert(!sf_storage_stop(),"device ownership releases after old address space teardown");sf_assert(!sf_storage_start()&&!sf_storage_mount(),"block driver and filesystem restart cleanly");
 int held=sf_ns_open(ns,1,"/","/disk/etc/readme.txt",1);sf_assert(held>0&&!sf_sys(SYS_KILL,driver_pid,SF_DEVICE_GONE,0),"driver death invokes kernel device reset");sf_persist_driver_down();char text[64];sf_assert(sf_ns_read(ns,1,held,text,sizeof(text))==SF_DEVICE_GONE,"dead driver invalidates cached file access");sf_ns_close(ns,1,held);sf_assert(!sf_storage_unmount()&&!sf_storage_stop()&&!sf_storage_start()&&!sf_storage_mount(),"confirmed reset allows isolated driver recovery");sf_log("STORAGE READY\n");
}
