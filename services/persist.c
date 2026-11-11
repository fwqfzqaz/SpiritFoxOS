#include "libsf.h"
#include "persist_server.h"
#include "txn.h"
#include "trust.h"
/* sf-persist.sfmod: the transaction volume, update state machine and block
 * access live in this process. sf-init owns the virtio-blk process and transfers
 * derived endpoint capabilities here; the block service accepts any holder of
 * that capability, so persistence never needs root-only task inspection. */
static uint32_t driver_ep,driver_reply,driver_pid;
static uint64_t capacity,sequence=0x200000000ull;
static int writable,driver_alive;
static uint8_t revoked[SF_TRUST_MAX*32];
static long revoked_bytes=-1;
static int call(SfMessage*m){
 if(!driver_alive||!driver_ep||!driver_reply)return SF_DEVICE_GONE;
 uint64_t id=++sequence;m->h.request_id=id;int r=sf_send(driver_ep,m);if(r)return r;
 long from=sf_recv(driver_reply,m,sf_deadline(2000));if(from<0){driver_alive=0;return(int)from;}
 if(from!=(long)driver_pid||m->h.request_id!=id)return SF_STALE_HANDLE;return(int)m->status;
}
int sf_storage_writable(void){return writable&&driver_alive&&driver_ep&&driver_reply;}
/* The dirty lease belongs to sf-init's storage ledger; this process only
 * performs the sector writes, so the transaction bracket is a no-op here. */
int sf_storage_transaction(int begin){(void)begin;return 0;}
static int sector_read(void*context,uint64_t lba,uint8_t out[512]){
 if(lba>=capacity/512)return SF_INVALID;
 for(unsigned half=0;half<512;half+=256){SfMessage m;sf_message(&m,OP_BLOCK_READ,0);m.offset=lba*512;m.mode=half;m.length=256;int r=call(&m);if(r)return r;if(m.length!=256)return SF_BAD_FORMAT;memcpy(out+half,m.data,256);}return 0;
}
int sf_storage_sector_read(void*c,uint64_t lba,uint8_t out[512]){return sector_read(c,lba,out);}
int sf_storage_sector_write(void*c,uint64_t lba,const uint8_t data[512]){
 if(!sf_storage_writable())return SF_READ_ONLY;
 if(lba>=capacity/512)return SF_INVALID;
 for(unsigned half=0;half<512;half+=256){SfMessage m;sf_message(&m,OP_BLOCK_WRITE,0);m.offset=lba*512;m.mode=half;m.length=256;memcpy(m.data,data+half,256);int r=call(&m);if(r)return r;}return 0;
}
int sf_storage_flush(void*c){SfMessage m;sf_message(&m,OP_BLOCK_FLUSH,0);return call(&m);}
int sf_storage_write_range(uint64_t first,uint64_t end){SfMessage m;sf_message(&m,OP_BLOCK_WRITE_RANGE,0);m.offset=first;m.handle=end;return call(&m);}
static int dispatch(SfMessage*m){
 long n;switch(m->h.opcode){
 case OP_PERSIST_ATTACH_SEND:
  if(!m->handle)break;
  if(driver_ep)sf_cap_close(driver_ep);
  driver_ep=(uint32_t)m->handle;driver_pid=(uint32_t)m->offset;driver_alive=1;return 0;
 case OP_PERSIST_ATTACH_REPLY:
  if(!m->handle)break;
  if(driver_reply)sf_cap_close(driver_reply);
  /* handle 已被内核用作能力字段，capacity 由 offset 承载。 */
  driver_reply=(uint32_t)m->handle;capacity=m->offset;writable=m->mode&1;driver_alive=1;return 0;
 case OP_PERSIST_DRIVER_DOWN:driver_alive=0;return 0;
 case OP_PERSIST_MOUNT:{
  uint64_t notes=0;int r=persist_mount(&notes);revoked_bytes=-1;
  if(r<0)return r;m->handle=notes;return 0;}
 case OP_PERSIST_READ:
  if(m->length>sizeof(m->data))return SF_INVALID;
  n=persist_notes_read(m->offset,m->data,m->length);if(n<0)return(int)n;m->length=n;return 0;
 case OP_PERSIST_WRITE:
  if(m->length>sizeof(m->data))return SF_INVALID;
  return(int)persist_notes_write(m->offset,m->data,m->length);
 case OP_PERSIST_TRUNCATE:return persist_notes_truncate(m->offset);
 case OP_PERSIST_STAGE:
  if(m->length>sizeof(m->data))return SF_INVALID;
  return persist_stage(m->offset,m->data,m->length);
 case OP_PERSIST_INSTALL:return persist_install();
 case OP_PERSIST_BEGIN:return persist_begin();
 case OP_PERSIST_FINISH:return persist_finish(m->mode&1);
 case OP_PERSIST_REVOKE:n=persist_revoke();if(n>=0)revoked_bytes=-1;return(int)n;
 case OP_PERSIST_STATUS:persist_status();return 0;
 case OP_PERSIST_PACKAGE:
 case OP_PERSIST_PREVIEW:{
  long total=persist_export(m->h.opcode==OP_PERSIST_PREVIEW);if(total<0)return(int)total;
  uint64_t offset=m->offset;if(offset>(uint64_t)total)offset=total;
  uint32_t chunk=(uint32_t)(total-offset);if(chunk>sizeof(m->data))chunk=sizeof(m->data);
  memcpy(m->data,persist_export_data()+offset,chunk);m->length=chunk;m->handle=(uint64_t)total;return 0;}
 case OP_PERSIST_REVOKED:{
  if(m->offset==0)revoked_bytes=persist_revoked_list(revoked,sizeof(revoked));
  if(revoked_bytes<0)return(int)revoked_bytes;
  uint64_t offset=m->offset;if(offset>(uint64_t)revoked_bytes)offset=revoked_bytes;
  uint32_t chunk=(uint32_t)(revoked_bytes-offset);if(chunk>sizeof(m->data))chunk=sizeof(m->data);
  memcpy(m->data,revoked+offset,chunk);m->length=chunk;m->handle=(uint64_t)revoked_bytes;return 0;}
 }
 return SF_INVALID;
}
long sf_main(const SfBootArgs*a,size_t size){
 if(size!=sizeof(*a)||a->abi_major!=1||!a->service||!a->manager)return SF_ABI_MISMATCH;
 if(sf_trust_init(&sf_trust,&sf_trust_store))return SF_INVALID;
 sf_log("[PERSIST] transactional volume service in ring 3\n");
 for(;;){
  SfMessage m;long from=sf_recv(a->service,&m,0);if(from<0)return from;
  if(m.h.opcode==OP_PERSIST_NOTES_SIZE){
   /* A metadata request never consumes a transferred capability. Close the
    * installed handle even when its sender or request is refused. */
   if(m.h.cap_count==1)sf_cap_close(m.handle);
   m.status=persist_metadata_dispatch(from,&m);
  }else m.status=from==1?dispatch(&m):SF_DENIED;
  /* The received message reports cap_count=1 while the newly installed handle
   * stays in m.handle. Replying with the same header would delegate that
   * driver capability back to the manager on every attach. */
  m.h.cap_count=0;
  sf_send(a->manager,&m);
 }
}
