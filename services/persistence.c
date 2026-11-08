#include "libsf.h"
#include "txn.h"
#include "storage.h"
#include "persist_server.h"
#include "update.h"
#include "version.h"
#include "trust.h"
/* This file is linked only into sf-persist.sfmod. The transaction volume and
 * the update state machine execute in that process; sf-init reaches them
 * through the bounded persist RPC in include/persistence.h. */
static SfTxn volume;
static uint8_t scratch[SF_TXN_FILE_BYTES];
static uint8_t stage[SF_TXN_FILE_BYTES];
static size_t stage_bytes;
static uint8_t export[SF_TXN_FILE_BYTES];
static long export_bytes=-1;
static int export_preview=-1;
static int commit(const char*const*paths,const void*const*data,const uint32_t*bytes,unsigned count){
 int r=sf_storage_transaction(1);if(r)return r;r=sf_txn_commit(&volume,paths,data,bytes,count);sf_storage_transaction(0);export_bytes=-1;export_preview=-1;return r;
}
static const char*slot_path(char slot){return slot=='a'?"/slots/a.sfk":"/slots/b.sfk";}
static int package_policy(const void*data,size_t bytes,char id[64],SfVersion*version,int enforce,const uint8_t**key_id){
 /* Accounting may tolerate runtime revocation after authenticating the package,
  * but immutable revoked keys and wrong-purpose signers always fail closed. */
 SfPackage p;int r=sf_package_open_trusted(&p,data,bytes,&sf_trust_store);if(r)return r;char runtime[64];
 if(enforce&&sf_trust_revoked(&sf_trust,p.key_id))return SF_REVOKED;
 if(key_id)*key_id=p.key_id;
 if(p.kind!=2||sf_manifest_text(&p,"runtime_id",runtime,sizeof(runtime))||strcmp(runtime,"org.spiritfox.native"))return SF_DEPENDENCY;
 memset(id,0,64);r=sf_manifest_text(&p,"app_id",id,64);if(r)return r;
 char text[33];r=sf_manifest_text(&p,"version",text,sizeof(text));return r?r:sf_version_parse(version,text);
}
/* The signed, digest-bound confirmed package is the version floor. No second
 * counter can advance before the existing health-confirmation transaction. */
static int confirmed_version(const SfUpdate*s,SfVersion*version){
 long n=sf_txn_get(&volume,slot_path(s->good),scratch,sizeof(scratch));if(n<0)return n;uint8_t hash[32];sf_sha256(hash,scratch,n);
 if(memcmp(hash,s->good_hash,32))return SF_BAD_SIGNATURE;char id[64];int r=package_policy(scratch,n,id,version,0,0);if(r)return r;return strcmp(id,s->app_id)?SF_DEPENDENCY:0;
}
static int state_read(SfUpdate*s){
 uint8_t bytes[SF_UPDATE_BYTES];long n=sf_txn_get(&volume,"/boot/current",bytes,sizeof(bytes));
 if(n==SF_NOT_FOUND){memset(s,0,sizeof(*s));return 0;}
 /* Import the original selector only after authenticating its selected app. */
 if(n==1&&(bytes[0]=='a'||bytes[0]=='b')){
  memset(s,0,sizeof(*s));s->good=bytes[0];n=sf_txn_get(&volume,slot_path(s->good),scratch,sizeof(scratch));if(n<0)return n;
  SfVersion version;int r=package_policy(scratch,n,s->app_id,&version,0,0);if(r)return r;sf_sha256(s->good_hash,scratch,n);
  return sf_update_encode(s,bytes);
 }
 return n<0?n:sf_update_decode(s,bytes,n);
}
static int state_commit(const SfUpdate*s){
 uint8_t encoded[SF_UPDATE_BYTES];int r=sf_update_encode(s,encoded);if(r)return r;
 const char*paths[]={"/boot/current"};const void*data[]={encoded};uint32_t lengths[]={sizeof(encoded)};return commit(paths,data,lengths,1);
}
long persist_notes_read(uint64_t offset,void*out,size_t bytes){
 if(!sf_storage_writable())return SF_DEVICE_GONE;long n=sf_txn_get(&volume,"/notes/user",scratch,sizeof(scratch));if(n==SF_NOT_FOUND)return 0;if(n<0)return n;
 if(offset>=(uint64_t)n)return 0;if(bytes>(uint64_t)n-offset)bytes=n-offset;memcpy(out,scratch+offset,bytes);return bytes;
}
long persist_notes_write(uint64_t offset,const void*data,size_t bytes){
 if(!sf_storage_writable())return SF_DEVICE_GONE;if(offset>sizeof(scratch)||bytes>sizeof(scratch)-offset)return SF_NO_SPACE;
 long old=sf_txn_get(&volume,"/notes/user",scratch,sizeof(scratch));if(old<0&&old!=SF_NOT_FOUND)return old;if(old<0)old=0;
 if(offset>(uint64_t)old)memset(scratch+old,0,offset-old);memcpy(scratch+offset,data,bytes);
 uint32_t length=offset+bytes;if(length<(uint64_t)old)length=old;const char*paths[]={"/notes/user"};const void*items[]={scratch};int r=commit(paths,items,&length,1);return r?r:(long)bytes;
}
int persist_notes_truncate(uint64_t bytes){
 if(!sf_storage_writable()||!volume.ready)return SF_DEVICE_GONE;
 int r=sf_storage_transaction(1);if(r)return r;r=sf_txn_resize(&volume,"/notes/user",bytes);sf_storage_transaction(0);return r;
}
int persist_mount(uint64_t*notes){
 volume.ready=0;export_bytes=-1;export_preview=-1;if(!sf_storage_writable())return SF_READ_ONLY;
 int r=sf_txn_mount(&volume,sf_storage_sector_read,sf_storage_sector_write,sf_storage_flush,0);if(r)return r;
 r=sf_storage_write_range(SF_TXN_HEADER+1,SF_TXN_BANK+2*SF_TXN_BANK_SECTORS);if(r){volume.ready=0;return r;}
 /* Load persisted key revocations before any package is verified again, so
  * a revocation committed before shutdown applies immediately after restart. */
 long revoked=sf_txn_get(&volume,"/boot/revoked",scratch,sizeof(scratch));
 if(revoked<0&&revoked!=SF_NOT_FOUND){volume.ready=0;return revoked;}
 if(revoked>0&&revoked%32){volume.ready=0;return SF_BAD_FORMAT;}
 for(long off=0;off<revoked;off+=32)if(sf_trust_revoke(&sf_trust,scratch+off)){volume.ready=0;return SF_NO_SPACE;}
 SfUpdate state;r=state_read(&state);if(r){volume.ready=0;return r;}
 if(state.phase==SF_UPDATE_TRYING){sf_update_finish(&state,0);r=state_commit(&state);if(r){volume.ready=0;return r;}sf_log("update: RECOVERED interrupted trial\n");}
 long n=sf_txn_get(&volume,"/notes/user",scratch,sizeof(scratch));if(n<0&&n!=SF_NOT_FOUND){volume.ready=0;return n;}
 if(notes)*notes=n<0?0:n;
 sf_log("PERSIST READY generation=");sf_number(volume.generation);sf_log("\n");return 0;
}
int persist_volume_ready(void){return volume.ready&&sf_storage_writable();}
int persist_stage(uint64_t offset,const void*data,size_t bytes){
 if(offset>sizeof(stage)||bytes>sizeof(stage)-offset)return SF_NO_SPACE;
 if(offset==0)stage_bytes=0;memcpy(stage+offset,data,bytes);
 if(offset+bytes>stage_bytes)stage_bytes=offset+bytes;return 0;
}
size_t persist_staged_bytes(void){return stage_bytes;}
int persist_install(void){
 if(!sf_storage_writable()||!volume.ready)return SF_DEVICE_GONE;
 if(!stage_bytes)return SF_BAD_FORMAT;
 SfUpdate state;int r=state_read(&state);if(r)return r;if(state.phase!=SF_UPDATE_STABLE)return SF_BUSY;
 SfVersion floor,version;if(state.good){r=confirmed_version(&state,&floor);if(r)return r;}
 char id[64];r=package_policy(stage,stage_bytes,id,&version,1,0);if(r)return r;uint8_t hash[32],encoded[SF_UPDATE_BYTES];sf_sha256(hash,stage,stage_bytes);
 if(state.good){if(strcmp(state.app_id,id))return SF_DEPENDENCY;r=sf_version_upgrade(&floor,state.good_hash,&version,hash);if(r)return r;}
 r=sf_update_stage(&state,id,hash);if(r)return r;r=sf_update_encode(&state,encoded);if(r)return r;
 const char*paths[]={slot_path(state.candidate),"/boot/current"};const void*data[]={stage,encoded};uint32_t lengths[]={stage_bytes,sizeof(encoded)};
 return commit(paths,data,lengths,2);
}
int persist_begin(void){
 if(!sf_storage_writable())return SF_DEVICE_GONE;SfUpdate state;int r=state_read(&state);if(r)return r;
 if(state.phase==SF_UPDATE_STABLE)return state.good?0:SF_NOT_FOUND;
 r=sf_update_begin(&state);if(r)return r;r=state_commit(&state);if(r)return r;
 sf_log("update: TRIAL started\n");return 1;
}
int persist_finish(int success){
 if(!sf_storage_writable())return SF_DEVICE_GONE;SfUpdate state;int r=state_read(&state);if(r)return r;
 r=sf_update_finish(&state,success);if(r)return r;r=state_commit(&state);if(r){sf_log("update: health commit failed\n");return r;}
 sf_log(success?"update: CONFIRMED\n":"update: ROLLED BACK\n");return 0;
}
/* Revoke the anchor key that signed a staged package. The package itself must
 * carry a valid anchor signature; revocation then outlives restarts through
 * the transaction volume. 1 means the signer was already revoked. */
int persist_revoke(void){
 if(!sf_storage_writable()||!volume.ready)return SF_DEVICE_GONE;
 if(!stage_bytes)return SF_BAD_FORMAT;
 SfPackage p;int r=sf_trust_open(&sf_trust,&p,stage,stage_bytes);if(r==SF_REVOKED)return 1;if(r)return r;
 if(sf_trust_revoked(&sf_trust,p.key_id))return 1;
 uint8_t list[SF_TRUST_MAX*32];long old=sf_txn_get(&volume,"/boot/revoked",list,sizeof(list));
 if(old<0&&old!=SF_NOT_FOUND)return old;if(old<0)old=0;
 if(old%32||old+32>(long)sizeof(list))return SF_BAD_FORMAT;
 if(sf_trust_revoke(&sf_trust,p.key_id))return SF_NO_SPACE;
 memcpy(list+old,p.key_id,32);
 const char*paths[]={"/boot/revoked"};const void*data[]={list};uint32_t lengths[]={old+32};
 return commit(paths,data,lengths,1);
}
static long selected_package(void*out,size_t capacity,int preview){
 if(!sf_storage_writable())return SF_DEVICE_GONE;SfUpdate state;int r=state_read(&state);if(r)return r;
 if(state.phase==SF_UPDATE_PENDING&&!preview)return SF_BUSY;int candidate=state.phase!=SF_UPDATE_STABLE;char selected=candidate?state.candidate:state.good;if(!selected)return SF_NOT_FOUND;
 long n=sf_txn_get(&volume,slot_path(selected),out,capacity);if(n<0)return n;uint8_t hash[32];sf_sha256(hash,out,n);
 if(memcmp(hash,candidate?state.candidate_hash:state.good_hash,32))return SF_BAD_SIGNATURE;
 char id[64];SfVersion version;r=package_policy(out,n,id,&version,1,0);if(r)return r;if(strcmp(id,state.app_id))return SF_DEPENDENCY;
 /* Recheck pending records created by an older implementation, too. */
 if(candidate&&state.good){SfVersion floor;r=confirmed_version(&state,&floor);if(r)return r;r=sf_version_upgrade(&floor,state.good_hash,&version,hash);if(r<0)return r;}
 return n;
}
/* The RPC layer exports the selected package in 256-byte chunks; the cache is
 * invalidated by every commit so a chunk sequence always reflects one state. */
long persist_export(int preview){
 if(export_bytes>=0&&export_preview==preview)return export_bytes;
 long n=selected_package(export,sizeof(export),preview);
 if(n>=0){export_bytes=n;export_preview=preview;}else{export_bytes=-1;export_preview=-1;}
 return n;
}
const uint8_t* persist_export_data(void){return export;}
long persist_revoked_list(void*out,size_t capacity){
 if(!volume.ready)return SF_DEVICE_GONE;
 long n=sf_txn_get(&volume,"/boot/revoked",out,capacity);return n==SF_NOT_FOUND?0:n;
}
void persist_status(void){
 sf_log(volume.ready&&sf_storage_writable()?"persist: ACTIVE generation=":"persist: UNAVAILABLE generation=");sf_number(volume.generation);sf_log("\n");
 SfUpdate state;if(!sf_storage_writable()||state_read(&state))return;
 sf_log(state.phase==SF_UPDATE_STABLE?"update: STABLE good=":state.phase==SF_UPDATE_PENDING?"update: PENDING good=":"update: TRYING good=");
 char text[]={state.good?state.good:'-',0};sf_log(text);sf_log(" candidate=");text[0]=state.candidate?state.candidate:'-';sf_log(text);sf_log("\n");
 if(state.good){SfVersion version;int r=confirmed_version(&state,&version);if(r){sf_log("update: confirmed-version unavailable code=");sf_number(-r);sf_log("\n");return;}
  sf_log("update: confirmed-version=");for(unsigned i=0;i<3;i++){if(i)sf_log(".");sf_number(version.part[i]);}sf_log(" policy=monotonic\n");}
}
