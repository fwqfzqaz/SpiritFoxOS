/* E1: real signed package parsing/signature verification and pure image
 * preparation. Fixture filesystem access here is host-test I/O, not a claim
 * that the production preparation helper performs I/O. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "persist_candidate.h"

static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"PERSIST CANDIDATE IMAGE FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)

typedef struct {
 char name[32];size_t bytes;uint8_t digest[32],key_id[32],*data;
} Fixture;

static void path_join(char*out,size_t size,const char*dir,const char*name){
 int n=snprintf(out,size,"%s/%s",dir,name);CHECK(n>0&&(size_t)n<size);
}
static uint8_t *read_file(const char*path,size_t*bytes){
 FILE*f=fopen(path,"rb");CHECK(f!=0);CHECK(!fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>=0);CHECK(!fseek(f,0,SEEK_SET));
 uint8_t*p=malloc(n?(size_t)n:1);CHECK(p!=0);CHECK(fread(p,1,(size_t)n,f)==(size_t)n);CHECK(!fclose(f));*bytes=(size_t)n;return p;
}
static void from_hex(uint8_t*out,const char*text){
 for(unsigned i=0;i<32;i++){unsigned value=0;CHECK(sscanf(text+i*2,"%2x",&value)==1);out[i]=(uint8_t)value;}
}
static void zero_output(const SfPersistCandidateImage*out){
 SfPersistCandidateImage zero;memset(&zero,0,sizeof(zero));CHECK(!memcmp(out,&zero,sizeof(zero)));
}
static int expected_status(const char*name){
 if(!strcmp(name,"valid"))return SF_OK;
 if(!strcmp(name,"wrong_id")||!strcmp(name,"wrong_category")||!strcmp(name,"app_kind")||!strcmp(name,"module_wrong_usage"))return SF_DENIED;
 if(!strcmp(name,"bad_signature"))return SF_BAD_SIGNATURE;
 if(!strcmp(name,"truncated"))return SF_BAD_FORMAT;
 if(!strcmp(name,"relative"))return SF_DEPENDENCY;
 CHECK(0);return SF_INVALID;
}
static int trust_open_expected(const char*name){
 return strcmp(name,"module_wrong_usage")&&strcmp(name,"bad_signature")&&strcmp(name,"truncated");
}

int main(int argc,char**argv){
 CHECK(argc==2);const char*dir=argv[1];char path[1024];
 uint8_t*dev_pub,*app_pub;size_t dev_bytes,app_bytes;
 path_join(path,sizeof(path),dir,"development.pub");dev_pub=read_file(path,&dev_bytes);
 path_join(path,sizeof(path),dir,"application.pub");app_pub=read_file(path,&app_bytes);
 CHECK(dev_bytes==32&&app_bytes==32&&memcmp(dev_pub,app_pub,32));
 SfTrustKey keys[2];memset(keys,0,sizeof(keys));memcpy(keys[0].public_key,dev_pub,32);keys[0].usage=SF_TRUST_MODULE;
 memcpy(keys[1].public_key,app_pub,32);keys[1].usage=SF_TRUST_APP;
 SfTrustStore policy={keys,2};SfRuntimeTrust trust;CHECK(!sf_trust_init(&trust,&policy));

 Fixture fixtures[8];memset(fixtures,0,sizeof(fixtures));unsigned count=0;
 path_join(path,sizeof(path),dir,"expected.txt");FILE*list=fopen(path,"r");CHECK(list!=0);
 while(count<8){
  char digest_text[65],key_text[65];unsigned long long bytes=0;
  int n=fscanf(list,"%31s %llu %64s %64s",fixtures[count].name,&bytes,digest_text,key_text);
  if(n==EOF)break;CHECK(n==4&&bytes>0&&bytes<=SF_PACKAGE_MAX);
  fixtures[count].bytes=(size_t)bytes;from_hex(fixtures[count].digest,digest_text);from_hex(fixtures[count].key_id,key_text);
  char filename[64];int written=snprintf(filename,sizeof(filename),"%s.pkg",fixtures[count].name);CHECK(written>0&&(size_t)written<sizeof(filename));
  path_join(path,sizeof(path),dir,filename);fixtures[count].data=read_file(path,&fixtures[count].bytes);
  CHECK(fixtures[count].bytes==(size_t)bytes);
  uint8_t digest[32];sf_sha256(digest,fixtures[count].data,fixtures[count].bytes);
  CHECK(!memcmp(digest,fixtures[count].digest,32));
  count++;
 }
 CHECK(!fclose(list)&&count==8);

 uint8_t*staging=malloc(SF_IMAGE_MAX);CHECK(staging!=0);
 SfPersistCandidateImage out;
 const Fixture*valid=0;unsigned valid_count=0;
 for(unsigned i=0;i<count;i++){
  Fixture*f=&fixtures[i];int status=expected_status(f->name);
  SfPackage opened;int open=sf_trust_open(&trust,&opened,f->data,f->bytes);
  if(trust_open_expected(f->name))CHECK(!open&&opened.key_id!=0);
  else CHECK(open<0);
  memset(staging,0xa5,SF_IMAGE_MAX);memset(&out,0xa5,sizeof(out));
  int r=sf_persist_candidate_prepare(&trust,f->data,f->bytes,staging,SF_IMAGE_MAX,&out);
  CHECK(r==status);
  if(!status){
   valid=f;valid_count++;
   CHECK(out.bytes==f->bytes&&!memcmp(out.source_sha256,f->digest,32));
   CHECK(!memcmp(out.key_id,f->key_id,32));
   CHECK(out.plan.size==sizeof(out.plan)&&out.plan.count>0&&out.plan.count<=8&&out.plan.image_base==SF_BASE);
   CHECK(!out.plan.grant_manager&&!out.plan.grant_service&&!out.plan.service_rights&&
         !out.plan.grant_device&&!out.plan.device_rights&&!out.plan.grant_bundle);
   SfArguments empty;memset(&empty,0,sizeof(empty));CHECK(!memcmp(&out.plan.arguments,&empty,sizeof(empty)));
   for(unsigned j=0;j<out.plan.count;j++)CHECK(out.plan.segments[j].data>= (uint64_t)(uintptr_t)staging&&
     out.plan.segments[j].data < (uint64_t)(uintptr_t)staging+SF_IMAGE_MAX);
  }else zero_output(&out);
 }
 CHECK(valid&&valid_count==1);

 /* Runtime revocation is applied by the real trust overlay. */
 SfRuntimeTrust revoked=trust;revoked.revoked=1;memcpy(revoked.revoked_ids[0],valid->key_id,32);
 memset(&out,0xa5,sizeof(out));CHECK(sf_persist_candidate_prepare(&revoked,valid->data,valid->bytes,staging,SF_IMAGE_MAX,&out)==SF_REVOKED);zero_output(&out);

 /* A disjoint valid request can fail image preflight; its output stays zero. */
 memset(&out,0xa5,sizeof(out));CHECK(sf_persist_candidate_prepare(&trust,valid->data,valid->bytes,staging,1,&out)==SF_NO_SPACE);zero_output(&out);

 /* Direct source/staging/output/header aliases reject before changing any bytes. */
 size_t alias_bytes=valid->bytes>SF_IMAGE_MAX?valid->bytes:SF_IMAGE_MAX;
 uint8_t*alias=malloc(alias_bytes);uint8_t*saved=malloc(alias_bytes);CHECK(alias&&saved);
 memcpy(alias,valid->data,valid->bytes);memset(alias+valid->bytes,0x6d,alias_bytes-valid->bytes);
 memcpy(saved,alias,alias_bytes);memset(&out,0xa5,sizeof(out));SfPersistCandidateImage out_saved=out;
 CHECK(sf_persist_candidate_prepare(&trust,alias,valid->bytes,alias,SF_IMAGE_MAX,&out)==SF_INVALID);
 CHECK(!memcmp(alias,saved,alias_bytes)&&!memcmp(&out,&out_saved,sizeof(out)));

 memcpy(alias,valid->data,valid->bytes);memset(alias+valid->bytes,0x6d,alias_bytes-valid->bytes);memcpy(saved,alias,alias_bytes);
 CHECK(sf_persist_candidate_prepare(&trust,valid->data,valid->bytes,alias,SF_IMAGE_MAX,(SfPersistCandidateImage*)alias)==SF_INVALID);
 CHECK(!memcmp(alias,saved,alias_bytes));

 memcpy(alias,valid->data,valid->bytes);memset(alias+valid->bytes,0x6d,alias_bytes-valid->bytes);memcpy(saved,alias,alias_bytes);
 CHECK(sf_persist_candidate_prepare(&trust,alias,valid->bytes,staging,SF_IMAGE_MAX,(SfPersistCandidateImage*)alias)==SF_INVALID);
 CHECK(!memcmp(alias,saved,alias_bytes));

 union { max_align_t align;SfRuntimeTrust trust;SfPersistCandidateImage out;uint8_t bytes[sizeof(SfPersistCandidateImage)]; } header_alias;
 memset(&header_alias,0x39,sizeof(header_alias));memcpy(&header_alias.trust,&trust,sizeof(trust));
 uint8_t header_saved[sizeof(header_alias)];memcpy(header_saved,&header_alias,sizeof(header_alias));
 CHECK(sf_persist_candidate_prepare(&header_alias.trust,valid->data,valid->bytes,staging,SF_IMAGE_MAX,&header_alias.out)==SF_INVALID);
 CHECK(!memcmp(header_saved,&header_alias,sizeof(header_alias)));

 /* The trust table is an indirect read range and must not alias caller spans. */
 memset(staging,0x47,SF_IMAGE_MAX);memcpy(staging,&keys[0],sizeof(keys[0]));memcpy(saved,staging,SF_IMAGE_MAX);
 SfRuntimeTrust indirect=trust;indirect.policy.keys=(const SfTrustKey*)staging;indirect.policy.count=1;
 memset(&out,0xa5,sizeof(out));out_saved=out;
 CHECK(sf_persist_candidate_prepare(&indirect,valid->data,valid->bytes,staging,SF_IMAGE_MAX,&out)==SF_INVALID);
 CHECK(!memcmp(staging,saved,SF_IMAGE_MAX)&&!memcmp(&out,&out_saved,sizeof(out)));

 SfRuntimeTrust out_table=trust;out_table.policy.keys=(const SfTrustKey*)&out;out_table.policy.count=1;
 memcpy(&out,&keys[0],sizeof(keys[0]));out_saved=out;
 CHECK(sf_persist_candidate_prepare(&out_table,valid->data,valid->bytes,staging,SF_IMAGE_MAX,&out)==SF_INVALID);
 CHECK(!memcmp(&out,&out_saved,sizeof(out)));

 SfRuntimeTrust source_table=trust;source_table.policy.keys=(const SfTrustKey*)valid->data;source_table.policy.count=1;
 memcpy(saved,valid->data,valid->bytes);memset(&out,0xa5,sizeof(out));out_saved=out;
 CHECK(sf_persist_candidate_prepare(&source_table,valid->data,valid->bytes,staging,SF_IMAGE_MAX,&out)==SF_INVALID);
 CHECK(!memcmp(valid->data,saved,valid->bytes)&&!memcmp(&out,&out_saved,sizeof(out)));

 /* Overflowing spans are rejected before dereference or writes. */
 memset(staging,0x5c,SF_IMAGE_MAX);memcpy(saved,staging,SF_IMAGE_MAX);memset(&out,0xa5,sizeof(out));out_saved=out;
 const void*bad_data=(const void*)(uintptr_t)(UINTPTR_MAX-8u);
 CHECK(sf_persist_candidate_prepare(&trust,bad_data,64,staging,SF_IMAGE_MAX,&out)==SF_INVALID);
 CHECK(!memcmp(staging,saved,SF_IMAGE_MAX)&&!memcmp(&out,&out_saved,sizeof(out)));
 void*bad_stage=(void*)(uintptr_t)(UINTPTR_MAX-8u);
 CHECK(sf_persist_candidate_prepare(&trust,valid->data,valid->bytes,bad_stage,64,&out)==SF_INVALID);
 CHECK(!memcmp(&out,&out_saved,sizeof(out)));
 SfPersistCandidateImage*bad_out=(SfPersistCandidateImage*)(uintptr_t)(UINTPTR_MAX-sizeof(SfPersistCandidateImage)+4u);
 CHECK(sf_persist_candidate_prepare(&trust,valid->data,valid->bytes,staging,SF_IMAGE_MAX,bad_out)==SF_INVALID);
 CHECK(!memcmp(staging,saved,SF_IMAGE_MAX));

 for(unsigned i=0;i<count;i++)free(fixtures[i].data);
 free(dev_pub);free(app_pub);free(staging);free(alias);free(saved);
 printf("PERSIST CANDIDATE IMAGE PASS %u checks; E1 real signed in-memory package/profile verification and image planning; host fixture I/O only\n",checks);
 return 0;
}
