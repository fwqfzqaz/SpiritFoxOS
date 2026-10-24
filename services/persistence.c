#include "libsf.h"
#include "txn.h"
#include "storage.h"
#include "persistence.h"
#include "update.h"
#include "version.h"
#include "trust.h"
static SfTxn volume;
static uint8_t scratch[SF_TXN_FILE_BYTES];
static long(*disk_read)(unsigned,uint64_t,void*,size_t);
static int commit(const char*const*paths,const void*const*data,const uint32_t*bytes,unsigned count){
 int r=sf_storage_transaction(1);if(r)return r;r=sf_txn_commit(&volume,paths,data,bytes,count);sf_storage_transaction(0);return r;
}
static const char*slot_path(char slot){return slot=='a'?"/slots/a.sfk":"/slots/b.sfk";}
static int package_policy(const void*data,size_t bytes,char id[64],SfVersion*version){
 SfPackage p;int r=sf_package_open_trusted(&p,data,bytes,&sf_trust_store);if(r)return r;char runtime[64];
 if(p.kind!=2||sf_manifest_text(&p,"runtime_id",runtime,sizeof(runtime))||strcmp(runtime,"org.spiritfox.native"))return SF_DEPENDENCY;
 memset(id,0,64);r=sf_manifest_text(&p,"app_id",id,64);if(r)return r;
 char text[33];r=sf_manifest_text(&p,"version",text,sizeof(text));return r?r:sf_version_parse(version,text);
}
/* The signed, digest-bound confirmed package is the version floor. No second
 * counter can advance before the existing health-confirmation transaction. */
static int confirmed_version(const SfUpdate*s,SfVersion*version){
 long n=sf_txn_get(&volume,slot_path(s->good),scratch,sizeof(scratch));if(n<0)return n;uint8_t hash[32];sf_sha256(hash,scratch,n);
 if(memcmp(hash,s->good_hash,32))return SF_BAD_SIGNATURE;char id[64];int r=package_policy(scratch,n,id,version);if(r)return r;return strcmp(id,s->app_id)?SF_DEPENDENCY:0;
}
static int state_read(SfUpdate*s){
 uint8_t bytes[SF_UPDATE_BYTES];long n=sf_txn_get(&volume,"/boot/current",bytes,sizeof(bytes));
 if(n==SF_NOT_FOUND){memset(s,0,sizeof(*s));return 0;}
 /* Import the original selector only after authenticating its selected app. */
 if(n==1&&(bytes[0]=='a'||bytes[0]=='b')){
  memset(s,0,sizeof(*s));s->good=bytes[0];n=sf_txn_get(&volume,slot_path(s->good),scratch,sizeof(scratch));if(n<0)return n;
  SfVersion version;int r=package_policy(scratch,n,s->app_id,&version);if(r)return r;sf_sha256(s->good_hash,scratch,n);
  return sf_update_encode(s,bytes);
 }
 return n<0?n:sf_update_decode(s,bytes,n);
}
static int state_commit(const SfUpdate*s){
 uint8_t encoded[SF_UPDATE_BYTES];int r=sf_update_encode(s,encoded);if(r)return r;
 const char*paths[]={"/boot/current"};const void*data[]={encoded};uint32_t lengths[]={sizeof(encoded)};return commit(paths,data,lengths,1);
}
static long notes_read(unsigned index,uint64_t offset,void*out,size_t bytes){
 if(index!=UINT32_MAX)return disk_read?disk_read(index,offset,out,bytes):SF_DEVICE_GONE;
 if(!sf_storage_writable())return SF_DEVICE_GONE;long n=sf_txn_get(&volume,"/notes/user",scratch,sizeof(scratch));if(n==SF_NOT_FOUND)return 0;if(n<0)return n;
 if(offset>=(uint64_t)n)return 0;if(bytes>(uint64_t)n-offset)bytes=n-offset;memcpy(out,scratch+offset,bytes);return bytes;
}
static long notes_write(unsigned index,uint64_t offset,const void*data,size_t bytes){
 if(index!=UINT32_MAX)return SF_READ_ONLY;if(!sf_storage_writable())return SF_DEVICE_GONE;if(offset>sizeof(scratch)||bytes>sizeof(scratch)-offset)return SF_NO_SPACE;
 long old=sf_txn_get(&volume,"/notes/user",scratch,sizeof(scratch));if(old<0&&old!=SF_NOT_FOUND)return old;if(old<0)old=0;
 if(offset>(uint64_t)old)memset(scratch+old,0,offset-old);memcpy(scratch+offset,data,bytes);
 uint32_t length=offset+bytes;if(length<(uint64_t)old)length=old;const char*paths[]={"/notes/user"};const void*items[]={scratch};int r=commit(paths,items,&length,1);return r?r:(long)bytes;
}
static int notes_truncate(unsigned index,uint64_t bytes){
 if(index!=UINT32_MAX)return SF_READ_ONLY;if(!sf_storage_writable()||!volume.ready)return SF_DEVICE_GONE;
 int r=sf_storage_transaction(1);if(r)return r;r=sf_txn_resize(&volume,"/notes/user",bytes);sf_storage_transaction(0);return r;
}
int sf_persist_mount(SfNamespace*ns){
 volume.ready=0;if(!sf_storage_writable())return SF_READ_ONLY;
 int r=sf_txn_mount(&volume,sf_storage_sector_read,sf_storage_sector_write,sf_storage_flush,0);if(r)return r;
 r=sf_storage_write_range(SF_TXN_HEADER+1,SF_TXN_BANK+2*SF_TXN_BANK_SECTORS);if(r){volume.ready=0;return r;}
 sf_storage_write_tests();
 SfUpdate state;r=state_read(&state);if(r){volume.ready=0;return r;}
 if(state.phase==SF_UPDATE_TRYING){sf_update_finish(&state,0);r=state_commit(&state);if(r){volume.ready=0;return r;}sf_log("update: RECOVERED interrupted trial\n");}
 long n=sf_txn_get(&volume,"/notes/user",scratch,sizeof(scratch));if(n<0&&n!=SF_NOT_FOUND){volume.ready=0;return n;}
 r=sf_ns_create(ns,"/persist",NODE_DIR,0);if(r){volume.ready=0;return r;}
 unsigned directory=0;for(unsigned i=0;i<32;i++)if(!strcmp(ns->nodes[i].path,"/persist"))directory=i;
 r=sf_ns_create(ns,"/persist/notes",NODE_DISK,0);if(r){memset(&ns->nodes[directory],0,sizeof(SfNode));volume.ready=0;return r;}
 for(unsigned i=0;i<32;i++)if(!strcmp(ns->nodes[i].path,"/persist/notes")){ns->nodes[i].external=UINT32_MAX;ns->nodes[i].mount_tag=1;ns->nodes[i].size=n<0?0:n;}
 ns->nodes[directory].readonly=1;ns->nodes[directory].mount_tag=1;
 if(ns->external_read!=notes_read)disk_read=ns->external_read;ns->external_read=notes_read;ns->external_write=notes_write;ns->external_truncate=notes_truncate;
 sf_log("PERSIST READY generation=");sf_number(volume.generation);sf_log("\n");return 0;
}
int sf_persist_install(const char*source){
 if(!sf_storage_writable()||!volume.ready)return SF_DEVICE_GONE;
 SfUpdate state;int r=state_read(&state);if(r)return r;if(state.phase!=SF_UPDATE_STABLE)return SF_BUSY;
 SfVersion floor,version;if(state.good){r=confirmed_version(&state,&floor);if(r)return r;}
 long n=sf_storage_load(source,scratch,sizeof(scratch));if(n<0)return n;
 char id[64];r=package_policy(scratch,n,id,&version);if(r)return r;uint8_t hash[32],encoded[SF_UPDATE_BYTES];sf_sha256(hash,scratch,n);
 if(state.good){if(strcmp(state.app_id,id))return SF_DEPENDENCY;r=sf_version_upgrade(&floor,state.good_hash,&version,hash);if(r)return r;}
 r=sf_update_stage(&state,id,hash);if(r)return r;r=sf_update_encode(&state,encoded);if(r)return r;
 const char*paths[]={slot_path(state.candidate),"/boot/current"};const void*data[]={scratch,encoded};uint32_t lengths[]={n,sizeof(encoded)};
 return commit(paths,data,lengths,2);
}
int sf_persist_begin(void){
 if(!sf_storage_writable())return SF_DEVICE_GONE;SfUpdate state;int r=state_read(&state);if(r)return r;
 if(state.phase==SF_UPDATE_STABLE)return state.good?0:SF_NOT_FOUND;
 r=sf_update_begin(&state);if(r)return r;r=state_commit(&state);if(r)return r;
 sf_log("update: TRIAL started\n");return 1;
}
int sf_persist_finish(int success){
 if(!sf_storage_writable())return SF_DEVICE_GONE;SfUpdate state;int r=state_read(&state);if(r)return r;
 r=sf_update_finish(&state,success);if(r)return r;r=state_commit(&state);if(r){sf_log("update: health commit failed\n");return r;}
 sf_log(success?"update: CONFIRMED\n":"update: ROLLED BACK\n");return 0;
}
static long selected_package(void*out,size_t capacity,int preview){
 if(!sf_storage_writable())return SF_DEVICE_GONE;SfUpdate state;int r=state_read(&state);if(r)return r;
 if(state.phase==SF_UPDATE_PENDING&&!preview)return SF_BUSY;int candidate=state.phase!=SF_UPDATE_STABLE;char selected=candidate?state.candidate:state.good;if(!selected)return SF_NOT_FOUND;
 long n=sf_txn_get(&volume,slot_path(selected),out,capacity);if(n<0)return n;uint8_t hash[32];sf_sha256(hash,out,n);
 if(memcmp(hash,candidate?state.candidate_hash:state.good_hash,32))return SF_BAD_SIGNATURE;
 char id[64];SfVersion version;r=package_policy(out,n,id,&version);if(r)return r;if(strcmp(id,state.app_id))return SF_DEPENDENCY;
 /* Recheck pending records created by an older implementation, too. */
 if(candidate&&state.good){SfVersion floor;r=confirmed_version(&state,&floor);if(r)return r;r=sf_version_upgrade(&floor,state.good_hash,&version,hash);if(r<0)return r;}
 return n;
}
long sf_persist_package(void*out,size_t capacity){return selected_package(out,capacity,0);}
long sf_persist_preview(void*out,size_t capacity){return selected_package(out,capacity,1);}
void sf_persist_status(void){
 sf_log(volume.ready&&sf_storage_writable()?"persist: ACTIVE generation=":"persist: UNAVAILABLE generation=");sf_number(volume.generation);sf_log("\n");
 SfUpdate state;if(!sf_storage_writable()||state_read(&state))return;
 sf_log(state.phase==SF_UPDATE_STABLE?"update: STABLE good=":state.phase==SF_UPDATE_PENDING?"update: PENDING good=":"update: TRYING good=");
 char text[]={state.good?state.good:'-',0};sf_log(text);sf_log(" candidate=");text[0]=state.candidate?state.candidate:'-';sf_log(text);sf_log("\n");
 if(state.good){SfVersion version;int r=confirmed_version(&state,&version);if(r){sf_log("update: confirmed-version unavailable code=");sf_number(-r);sf_log("\n");return;}
  sf_log("update: confirmed-version=");for(unsigned i=0;i<3;i++){if(i)sf_log(".");sf_number(version.part[i]);}sf_log(" policy=monotonic\n");}
}
