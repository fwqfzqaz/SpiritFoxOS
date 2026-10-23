#include "libsf.h"
#include "txn.h"
#include "storage.h"
#include "persistence.h"
#include "trust.h"
static SfTxn volume;
static uint8_t scratch[SF_TXN_FILE_BYTES];
static long(*disk_read)(unsigned,uint64_t,void*,size_t);
static int commit(const char*const*paths,const void*const*data,const uint32_t*bytes,unsigned count){
 int r=sf_storage_transaction(1);if(r)return r;r=sf_txn_commit(&volume,paths,data,bytes,count);sf_storage_transaction(0);return r;
}
static long notes_read(unsigned index,uint64_t offset,void*out,size_t bytes){
 if(index!=UINT32_MAX)return disk_read?disk_read(index,offset,out,bytes):SF_DEVICE_GONE;
 if(!sf_storage_writable())return SF_DEVICE_GONE;long n=sf_txn_get(&volume,"/notes/user",scratch,sizeof(scratch));if(n==SF_NOT_FOUND)return 0;if(n<0)return n;
 if(offset>=(uint64_t)n)return 0;if(bytes>(uint64_t)n-offset)bytes=n-offset;memcpy(out,scratch+offset,bytes);return bytes;
}
static long notes_write(unsigned index,uint64_t offset,const void*data,size_t bytes){
 if(index!=UINT32_MAX)return SF_READ_ONLY;if(offset>sizeof(scratch)||bytes>sizeof(scratch)-offset)return SF_NO_SPACE;
 long old=sf_txn_get(&volume,"/notes/user",scratch,sizeof(scratch));if(old<0&&old!=SF_NOT_FOUND)return old;if(old<0)old=0;
 if(offset>(uint64_t)old)memset(scratch+old,0,offset-old);memcpy(scratch+offset,data,bytes);
 uint32_t length=offset+bytes;if(length<(uint64_t)old)length=old;const char*paths[]={"/notes/user"};const void*items[]={scratch};int r=commit(paths,items,&length,1);return r?r:(long)bytes;
}
int sf_persist_mount(SfNamespace*ns){
 volume.ready=0;if(!sf_storage_writable())return SF_READ_ONLY;
 int r=sf_txn_mount(&volume,sf_storage_sector_read,sf_storage_sector_write,sf_storage_flush,0);if(r)return r;
 r=sf_storage_write_range(SF_TXN_HEADER+1,SF_TXN_BANK+2*SF_TXN_BANK_SECTORS);if(r){volume.ready=0;return r;}
 sf_storage_write_tests();
 if(ns->external_read!=notes_read)disk_read=ns->external_read;ns->external_read=notes_read;ns->external_write=notes_write;
 r=sf_ns_create(ns,"/persist",NODE_DIR,0);if(r){volume.ready=0;return r;}
 unsigned directory=0;for(unsigned i=0;i<32;i++)if(!strcmp(ns->nodes[i].path,"/persist"))directory=i;
 r=sf_ns_create(ns,"/persist/notes",NODE_DISK,0);if(r){memset(&ns->nodes[directory],0,sizeof(SfNode));volume.ready=0;return r;}
 long n=sf_txn_get(&volume,"/notes/user",scratch,sizeof(scratch));if(n<0&&n!=SF_NOT_FOUND)return n;
 for(unsigned i=0;i<32;i++)if(!strcmp(ns->nodes[i].path,"/persist/notes")){ns->nodes[i].external=UINT32_MAX;ns->nodes[i].mount_tag=1;ns->nodes[i].size=n<0?0:n;}
 ns->nodes[directory].readonly=1;ns->nodes[directory].mount_tag=1;
 sf_log("PERSIST READY generation=");sf_number(volume.generation);sf_log("\n");return 0;
}
static int current(char*out){char c;long n=sf_txn_get(&volume,"/boot/current",&c,1);if(n==SF_NOT_FOUND){*out=0;return 0;}if(n!=1||(c!='a'&&c!='b'))return SF_BAD_FORMAT;*out=c;return 0;}
int sf_persist_install(const char*source){
 if(!sf_storage_writable()||!volume.ready)return SF_DEVICE_GONE;
 long n=sf_storage_load(source,scratch,sizeof(scratch));if(n<0)return n;
 SfPackage p;int r=sf_package_open(&p,scratch,n,sf_trust_key);if(r)return r;char runtime[64];
 if(p.kind!=2||sf_manifest_text(&p,"runtime_id",runtime,sizeof(runtime))||strcmp(runtime,"org.spiritfox.native"))return SF_DEPENDENCY;
 char selected;r=current(&selected);if(r)return r;selected=selected=='a'?'b':'a';
 const char*paths[]={selected=='a'?"/slots/a.sfk":"/slots/b.sfk","/boot/current"};const void*data[]={scratch,&selected};uint32_t lengths[]={n,1};
 return commit(paths,data,lengths,2);
}
long sf_persist_package(void*out,size_t capacity){
 if(!sf_storage_writable())return SF_DEVICE_GONE;char selected;int r=current(&selected);if(r)return r;if(!selected)return SF_NOT_FOUND;
 return sf_txn_get(&volume,selected=='a'?"/slots/a.sfk":"/slots/b.sfk",out,capacity);
}
void sf_persist_status(void){sf_log(volume.ready&&sf_storage_writable()?"persist: ACTIVE generation=":"persist: UNAVAILABLE generation=");sf_number(volume.generation);sf_log("\n");}
