/* Real persistence/update/transaction code, with sector I/O in memory. No
 * syscall or fabricated transaction result substitutes for the volume. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LIBSF_H
#include "sf.h"
static void sf_log(const char*s){(void)s;}
static void sf_number(uint64_t n){(void)n;}
#include "../../services/persistence.c"
static unsigned checks,reads,writes,flushes,ranges,brackets;
static int device=1;
static uint8_t media[SF_TXN_SECTORS*512],media_copy[sizeof(media)];
static SfTxn volume_copy;
static SfRuntimeTrust trust_copy;
static uint8_t scratch_copy[sizeof(scratch)],stage_copy[sizeof(stage)],export_copy[sizeof(export)];
static size_t stage_length;static long export_length;static int export_kind;
static unsigned io_copy[5];
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"PERSIST VOLUME FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
int sf_storage_writable(void){return device;}
int sf_storage_transaction(int begin){(void)begin;brackets++;return 0;}
int sf_storage_sector_read(void*c,uint64_t lba,uint8_t out[512]){(void)c;reads++;if(lba>=SF_TXN_SECTORS)return SF_INVALID;memcpy(out,media+lba*512,512);return 0;}
int sf_storage_sector_write(void*c,uint64_t lba,const uint8_t in[512]){(void)c;writes++;if(lba>=SF_TXN_SECTORS)return SF_INVALID;memcpy(media+lba*512,in,512);return 0;}
int sf_storage_flush(void*c){(void)c;flushes++;return 0;}
int sf_storage_write_range(uint64_t first,uint64_t end){ranges++;return first==2049&&end==2563?0:SF_INVALID;}
void persist_fixture_snapshot(void){
 memcpy(media_copy,media,sizeof(media));volume_copy=volume;trust_copy=sf_trust;
 memcpy(scratch_copy,scratch,sizeof(scratch));memcpy(stage_copy,stage,sizeof(stage));memcpy(export_copy,export,sizeof(export));
 stage_length=stage_bytes;export_length=export_bytes;export_kind=export_preview;
 io_copy[0]=reads;io_copy[1]=writes;io_copy[2]=flushes;io_copy[3]=ranges;io_copy[4]=brackets;
}
int persist_fixture_unchanged(void){
 return !memcmp(media,media_copy,sizeof(media))&&!memcmp(&volume,&volume_copy,sizeof(volume))&&
  !memcmp(&sf_trust,&trust_copy,sizeof(sf_trust))&&!memcmp(scratch,scratch_copy,sizeof(scratch))&&
  !memcmp(stage,stage_copy,sizeof(stage))&&!memcmp(export,export_copy,sizeof(export))&&
  stage_bytes==stage_length&&export_bytes==export_length&&export_preview==export_kind&&
  reads==io_copy[0]&&writes==io_copy[1]&&flushes==io_copy[2]&&ranges==io_copy[3]&&brackets==io_copy[4];
}
void persist_fixture_device(int alive){device=alive;}
unsigned persist_fixture_brackets(void){return brackets;}
static size_t package(const char*name,uint8_t out[SF_TXN_FILE_BYTES]){
 FILE*f=fopen(name,"rb");CHECK(f);size_t n=fread(out,1,SF_TXN_FILE_BYTES,f);CHECK(n&&fgetc(f)==EOF);fclose(f);return n;
}
static void readonly(uint64_t expected){
 persist_fixture_snapshot();
 for(unsigned i=0;i<256;i++){uint64_t n=UINT64_MAX;CHECK(!persist_notes_size(&n)&&n==expected);}
 CHECK(persist_fixture_unchanged());
}
unsigned persist_volume_tests(void){
 uint64_t n=UINT64_MAX;CHECK(persist_notes_size(&n)==SF_DEVICE_GONE&&n==UINT64_MAX);CHECK(persist_notes_size(0)==SF_INVALID);
 FILE*f=fopen("build/persistent.img","rb");CHECK(f&&fread(media,1,sizeof(media),f)==sizeof(media));fclose(f);
 CHECK(!sf_trust_init(&sf_trust,&sf_trust_store));CHECK(!persist_mount(&n)&&n==0);readonly(0);
 static uint8_t bytes[SF_TXN_FILE_BYTES];for(unsigned i=0;i<sizeof(bytes);i++)bytes[i]=(uint8_t)(i*13u);
 CHECK(persist_notes_write(0,bytes,sizeof(bytes))==sizeof(bytes));readonly(sizeof(bytes));
 CHECK(!persist_notes_truncate(257));readonly(257);CHECK(!persist_notes_truncate(0));readonly(0);
 CHECK(persist_notes_write(0,"durable-note",12)==12);readonly(12);
 /* Enter an actual signed update trial using the production policy. */
 size_t len=package("build/diskhello.sfk",bytes);CHECK(!persist_stage(0,bytes,len)&&!persist_install());CHECK(persist_begin()==1&&!persist_finish(1));
 len=package("build/diskupdate.sfk",bytes);CHECK(!persist_stage(0,bytes,len)&&!persist_install());CHECK(persist_begin()==1);
 SfUpdate trying;CHECK(!state_read(&trying)&&trying.phase==SF_UPDATE_TRYING&&trying.good&&trying.candidate);
 /* A persisted unrelated-key revocation and populated export/staging caches must
  * remain byte-identical too. The active app key is still permitted. */
 uint8_t key[32];const uint8_t unused_key[]="metadata-fixture-key";sf_sha256(key,unused_key,sizeof(unused_key)-1);CHECK(!sf_trust_revoke(&sf_trust,key));
 const char*paths[]={"/boot/revoked"};const void*data[]={key};uint32_t sizes[]={32};CHECK(!commit(paths,data,sizes,1));
 CHECK(persist_export(0)>0);CHECK(!persist_stage(0,"untouched-stage",15));readonly(12);
 SfUpdate after;CHECK(!state_read(&after)&&!memcmp(&trying,&after,sizeof(trying)));
 uint64_t generation=volume.generation;
 /* Control: repeating mount really does roll back this same TRYING state. */
 CHECK(!persist_mount(&n)&&volume.generation==generation+1);CHECK(!state_read(&after)&&after.phase==SF_UPDATE_STABLE&&!after.candidate);
 CHECK(persist_begin()==0);len=package("build/diskupdate.sfk",bytes);CHECK(!persist_stage(0,bytes,len)&&!persist_install());CHECK(persist_begin()==1);
 CHECK(persist_export(0)>0);CHECK(!persist_stage(0,"client-stage",12));
 device=0;n=UINT64_MAX;persist_fixture_snapshot();CHECK(persist_notes_size(&n)==SF_DEVICE_GONE&&n==UINT64_MAX&&persist_fixture_unchanged());device=1;
 volume.ready=0;n=UINT64_MAX;CHECK(persist_notes_size(&n)==SF_DEVICE_GONE&&n==UINT64_MAX);volume.ready=1;
 persist_fixture_snapshot();return checks;
}
