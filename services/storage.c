#include "libsf.h"
#include "storage.h"
#include "virtio.h"
#include "txn.h"
static SfManager*manager;
static SfNamespace*ns;
static SfModuleSpawn spawn;
static uint32_t driver_reply,driver_ep,driver_pid,granted_device;
static int driver_slot=-1,fs_slot=-1,cache_index=-1;
static uint64_t capacity,sequence=0x100000000ull;
static SfReadFs fs;
static uint8_t verified[SF_READFS_FILE_MAX];
static int writable,write_grant;
static int call(SfMessage*m){
 if(!driver_pid||sf_sys(SYS_TASK_STATUS,driver_pid,0,0)!=SF_TASK_RUNNING)return SF_DEVICE_GONE;
 uint64_t id=++sequence;m->h.request_id=id;int r=sf_send(driver_ep,m);if(r)return r;
 long from=sf_recv(driver_reply,m,sf_deadline(2000));if(from<0){sf_sys(SYS_KILL,driver_pid,SF_DEVICE_GONE,0);return from;}
 if(from!=driver_pid||m->h.request_id!=id)return SF_STALE_HANDLE;return m->status;
}
static int sector(void*context,uint64_t lba,uint8_t out[512]){
 if(lba>=capacity/512)return SF_INVALID;
 for(unsigned half=0;half<512;half+=256){SfMessage m;sf_message(&m,OP_BLOCK_READ,0);m.offset=lba*512;m.mode=half;m.length=256;int r=call(&m);if(r)return r;if(m.length!=256)return SF_BAD_FORMAT;memcpy(out+half,m.data,256);}return 0;
}
int sf_storage_sector_read(void*c,uint64_t lba,uint8_t out[512]){return sector(c,lba,out);}
int sf_storage_writable(void){return writable&&fs.mounted&&driver_pid&&sf_sys(SYS_TASK_STATUS,driver_pid,0,0)==SF_TASK_RUNNING;}
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
static int mount_open(unsigned tag){return tag==1&&fs_slot>=0?sfmod_acquire(manager,fs_slot,RES_OPEN):SF_STALE_HANDLE;}
static void mount_close(unsigned tag){if(tag==1&&fs_slot>=0)sfmod_release(manager,fs_slot,RES_OPEN);}
static long external_read(unsigned index,uint64_t offset,void*out,size_t bytes){
 if(!fs.mounted||index>=fs.count||sf_sys(SYS_TASK_STATUS,driver_pid,0,0)!=SF_TASK_RUNNING)return SF_DEVICE_GONE;
 SfReadFile*f=&fs.files[index];if(offset>=f->bytes||!bytes)return 0;
 if(cache_index!=(int)index){long n=sf_readfs_load(&fs,index,verified,sizeof(verified));if(n<0){cache_index=-1;return n;}cache_index=index;}
 if(bytes>f->bytes-offset)bytes=f->bytes-offset;memcpy(out,verified+offset,bytes);return bytes;
}
static int namespace_node(const char*path,unsigned type,unsigned external,uint32_t size){
 int r=sf_ns_create(ns,path,type,type==NODE_DISK);if(r&&r!=SF_BUSY)return r;
 for(unsigned i=0;i<32;i++)if(ns->nodes[i].type&&!strcmp(ns->nodes[i].path,path)){
  SfNode*n=&ns->nodes[i];if(n->type!=type||(r==SF_BUSY&&strcmp(path,"/disk")&&n->mount_tag!=1))return SF_BUSY;n->external=external;n->size=size;n->mount_tag=1;return 0;}
 return SF_NO_SPACE;
}
void sf_storage_init(SfManager*m,SfNamespace*n,SfModuleSpawn callback){manager=m;ns=n;spawn=callback;ns->external_read=external_read;ns->mount_open=mount_open;ns->mount_close=mount_close;}
static int start_driver(unsigned rights){
 if(driver_pid&&sf_sys(SYS_TASK_STATUS,driver_pid,0,0)==SF_TASK_RUNNING)return SF_BUSY;
 uint32_t device=0;for(unsigned d=0;d<32&&!device;d++)for(unsigned f=0;f<8;f++){uint32_t bdf=d*8+f;long id=sf_sys(SYS_PCI_READ,bdf,0,0);if(id==0x10011af4u||id==0x10421af4u){device=bdf+1;break;}}
 if(!device)return SF_NOT_FOUND;
 if(driver_slot<0){driver_slot=sfmod_add(manager,"org.spiritfox.virtio.blk",0,0);if(driver_slot<0)return driver_slot;}
 int r=sfmod_start(manager,driver_slot);if(r)return r;driver_reply=sf_sys(SYS_EP_CREATE,0,0,0);if((int)driver_reply<0){r=(int)driver_reply;driver_reply=0;goto fail;}driver_ep=sf_sys(SYS_EP_CREATE,0,0,0);if((int)driver_ep<0){r=(int)driver_ep;goto fail;}
 int pid=spawn("sf-virtio-blk.sfmod",driver_ep,RIGHT_RECV,device,rights,driver_reply);if(pid<0){r=pid;goto fail;}driver_pid=pid;granted_device=device;
 SfMessage message;sf_message(&message,OP_BLOCK_INFO,0);r=call(&message);if(r)goto fail;
 if(message.length!=sizeof(SfBlockInfo)){r=SF_BAD_FORMAT;goto fail;}SfBlockInfo info;memcpy(&info,message.data,sizeof(info));if(info.sector_bytes!=512||info.readonly!=(rights?0u:1u)||!info.sectors||info.sectors>UINT64_MAX/512){r=SF_BAD_FORMAT;goto fail;}capacity=info.sectors*512;write_grant=rights!=0;writable=0;
 sfmod_acquire(manager,driver_slot,RES_DMA);return 0;
fail:
 if(driver_pid)sf_sys(SYS_KILL,driver_pid,SF_DEVICE_GONE,0);driver_pid=0;if((int)driver_ep>0)sf_sys(SYS_CAP_REVOKE,driver_ep,0,0);driver_ep=0;if(driver_reply)sf_sys(SYS_CAP_REVOKE,driver_reply,0,0);driver_reply=0;
 sfmod_prepare(manager,driver_slot);sfmod_stop(manager,driver_slot);return r;
}
int sf_storage_start(void){int r=start_driver(0);return r==SF_READ_ONLY?start_driver(SF_DEVICE_WRITE):r;}
int sf_storage_mount(void){
 if(fs.mounted)return SF_BUSY;if(!driver_pid)return SF_DEVICE_GONE;
 for(unsigned i=0;i<32;i++)if(ns->nodes[i].type&&strlen(ns->nodes[i].path)>5&&!memcmp(ns->nodes[i].path,"/disk/",6))return SF_BUSY;
 for(unsigned i=0;i<32;i++)if(ns->descriptions[i].refs){const char*p=ns->nodes[ns->descriptions[i].node].path;if(!strcmp(p,"/disk")||(strlen(p)>5&&!memcmp(p,"/disk/",6)))return SF_BUSY;}
 int r=sf_readfs_mount(&fs,sector,0,capacity);if(r)return r;cache_index=-1;
 if(write_grant){
  uint8_t header[512],hash[32];r=sector(0,SF_TXN_HEADER,header);if(r){fs.mounted=0;return r;}sf_sha256(hash,header,480);
  if(capacity!=SF_TXN_SECTORS*512ull||memcmp(header,"SFTXN1\0",8)||sf_u32(header+8)!=1||sf_u32(header+12)!=SF_TXN_SECTORS||memcmp(hash,header+480,32)){fs.mounted=0;return SF_BAD_FORMAT;}
  for(unsigned i=0;i<fs.count;i++)if(fs.files[i].offset+fs.files[i].bytes>SF_TXN_HEADER*512ull){fs.mounted=0;return SF_BAD_FORMAT;}
 }
 for(unsigned i=0;i<fs.count;i++){long n=sf_readfs_load(&fs,i,verified,sizeof(verified));if(n<0){fs.mounted=0;return n;}}
 if(fs_slot<0){fs_slot=sfmod_add(manager,"org.spiritfox.fs.read",1u<<driver_slot,0);if(fs_slot<0){fs.mounted=0;return fs_slot;}}
 r=sfmod_start(manager,fs_slot);if(r){fs.mounted=0;return r;}
 r=namespace_node("/disk",NODE_DIR,0,0);if(r)goto fail;
 for(unsigned i=0;i<fs.count;i++){
  char path[96]="/disk";memcpy(path+5,fs.files[i].path,strlen(fs.files[i].path)+1);
  for(char*p=path+6;*p;p++)if(*p=='/'){*p=0;r=namespace_node(path,NODE_DIR,0,0);*p='/';if(r)goto fail;}
  r=namespace_node(path,NODE_DISK,i,fs.files[i].bytes);if(r)goto fail;
 }
 for(unsigned i=0;i<32;i++)if(ns->nodes[i].mount_tag==1)ns->nodes[i].readonly=1;
 writable=write_grant;
 return 0;
fail:
 sf_ns_unmount(ns,1);sfmod_prepare(manager,fs_slot);sfmod_stop(manager,fs_slot);fs.mounted=0;return r;
}
int sf_storage_unmount(void){
 if(!fs.mounted||fs_slot<0)return SF_NOT_FOUND;int r=sfmod_prepare(manager,fs_slot);if(r)return r;
 if(manager->modules[fs_slot].resources[RES_OPEN]||manager->modules[fs_slot].resources[RES_DIRTY]){sfmod_cancel(manager,fs_slot);return SF_BUSY;}
 r=sf_ns_unmount(ns,1);if(r){sfmod_cancel(manager,fs_slot);return r;}r=sfmod_stop(manager,fs_slot);if(r)return r;fs.mounted=0;cache_index=-1;return 0;
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
 sfmod_release(manager,driver_slot,RES_DMA);r=sfmod_stop(manager,driver_slot);if(r)return r;sf_sys(SYS_KILL,driver_pid,0,0);sf_sys(SYS_CAP_REVOKE,driver_ep,0,0);sf_sys(SYS_CAP_REVOKE,driver_reply,0,0);driver_pid=driver_ep=driver_reply=0;return 0;
}
long sf_storage_load(const char*path,void*out,size_t capacity_bytes){
 if(strlen(path)<7||memcmp(path,"/disk/",6))return SF_INVALID;if(!fs.mounted||fs_slot<0)return SF_DEVICE_GONE;
 int r=sfmod_acquire(manager,fs_slot,RES_OPEN);if(r)return r;int index=sf_readfs_find(&fs,path+5);long result=index<0?index:sf_readfs_load(&fs,index,out,capacity_bytes);sfmod_release(manager,fs_slot,RES_OPEN);return result;
}
int sf_storage_ready(void){return fs.mounted;}
int sf_storage_crash_test(void){
 if(!sf_storage_writable())return SF_DEVICE_GONE;
 int h=sf_ns_open(ns,1,"/","/persist/notes",OPEN_READ|OPEN_WRITE);if(h<0)return h;
 char data[32];long r=sf_ns_read(ns,1,h,data,sizeof(data));if(r<0){sf_ns_close(ns,1,h);return r;}
 sf_assert(!sf_storage_transaction(1),"driver failure test owns dirty lease");
 sf_assert(!sf_sys(SYS_KILL,driver_pid,SF_BUSY,0),"driver failure uses exit code SF_BUSY");
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
 sf_assert(sf_sys(SYS_TASK_STATUS,driver_pid,0,0)==SF_DEVICE_GONE,"faulting DMA driver terminated");
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
 if(!fs.mounted||writable)return;SfMessage m;sf_message(&m,OP_BLOCK_READ,0);m.offset=capacity;m.length=256;sf_assert(call(&m)==SF_INVALID,"block read rejects capacity boundary");
 sf_message(&m,OP_BLOCK_READ,0);m.offset=1;m.length=256;sf_assert(call(&m)==SF_INVALID,"block read rejects unaligned offset");sf_message(&m,OP_BLOCK_WRITE,0);sf_assert(call(&m)==SF_READ_ONLY,"block service rejects every write");
 sf_assert(sf_storage_stop()==SF_DEPENDENCY,"mounted filesystem pins block driver");int h=sf_ns_open(ns,1,"/","/disk/etc/readme.txt",1);sf_assert(h>0&&sf_storage_unmount()==SF_BUSY,"open disk file prevents unmount and restores admission");int dup=sf_ns_dup(ns,1,h);sf_ns_close(ns,1,h);sf_assert(sf_storage_unmount()==SF_BUSY,"duplicated disk description retains mount lease");sf_ns_close(ns,1,dup);
 sf_assert(sf_ns_open(ns,1,"/","/disk/etc/readme.txt",2)==SF_READ_ONLY&&sf_ns_create(ns,"/disk/etc/new",NODE_FILE,0)==SF_READ_ONLY,"read-only mount rejects writes and creation");
 sf_assert(!sf_storage_unmount(),"unmount drains file references");sf_message(&m,OP_BLOCK_STOP,0);sf_assert(!call(&m),"driver quiesces before process teardown");
 int candidate_ep=sf_sys(SYS_EP_CREATE,0,0,0);sf_assert(candidate_ep>0,"replacement endpoint allocated");int replacement=spawn("sf-virtio-blk.sfmod",candidate_ep,RIGHT_RECV,granted_device,0,driver_reply);sf_assert(replacement==SF_BUSY,"live stopped driver cannot share DMA arena with replacement");sf_sys(SYS_CAP_REVOKE,candidate_ep,0,0);
 sf_assert(!sf_storage_stop(),"device ownership releases after old address space teardown");sf_assert(!sf_storage_start()&&!sf_storage_mount(),"block driver and filesystem restart cleanly");
 int held=sf_ns_open(ns,1,"/","/disk/etc/readme.txt",1);sf_assert(held>0&&!sf_sys(SYS_KILL,driver_pid,SF_DEVICE_GONE,0),"driver death invokes kernel device reset");char text[64];sf_assert(sf_ns_read(ns,1,held,text,sizeof(text))==SF_DEVICE_GONE,"dead driver invalidates cached file access");sf_ns_close(ns,1,held);sf_assert(!sf_storage_unmount()&&!sf_storage_stop()&&!sf_storage_start()&&!sf_storage_mount(),"confirmed reset allows isolated driver recovery");sf_log("STORAGE READY\n");
}
