#include "trust_fingerprint.h"

static int span_end(uintptr_t start,uint64_t bytes,uintptr_t*end){
 if(bytes>(uint64_t)(UINTPTR_MAX-start))return 0;
 *end=start+(uintptr_t)bytes;return 1;
}
static int overlap(uintptr_t a,uintptr_t ae,uintptr_t b,uintptr_t be){return a<be&&b<ae;}
static void put32(uint8_t*out,uint32_t value){
 out[0]=(uint8_t)value;out[1]=(uint8_t)(value>>8);out[2]=(uint8_t)(value>>16);out[3]=(uint8_t)(value>>24);
}
static int key_less(const SfTrustKey*keys,unsigned a,unsigned b){
 return memcmp(keys[a].public_key,keys[b].public_key,32)<0;
}
static int id_less(const SfRuntimeTrust*trust,unsigned a,unsigned b){
 return memcmp(trust->revoked_ids[a],trust->revoked_ids[b],32)<0;
}

int sf_trust_fingerprint(const SfRuntimeTrust*trust,uint8_t out[32]){
 if(!trust||!out)return SF_INVALID;
 uintptr_t trust_start=(uintptr_t)trust,out_start=(uintptr_t)out,trust_end,out_end;
 if(!span_end(trust_start,sizeof(*trust),&trust_end)||!span_end(out_start,32,&out_end))return SF_INVALID;
 if(overlap(trust_start,trust_end,out_start,out_end))return SF_INVALID;

 const SfTrustStore*policy=&trust->policy;
 if(!policy->keys||!policy->count||policy->count>SF_TRUST_MAX)return SF_BAD_FORMAT;
 uintptr_t keys_start=(uintptr_t)policy->keys,keys_end;
 if(!span_end(keys_start,(uint64_t)policy->count*sizeof(SfTrustKey),&keys_end))return SF_INVALID;
 if(overlap(keys_start,keys_end,out_start,out_end)||overlap(keys_start,keys_end,trust_start,trust_end))return SF_INVALID;
 if(trust->revoked>SF_TRUST_MAX)return SF_BAD_FORMAT;
 if(sf_trust_validate(policy))return SF_BAD_FORMAT;
 for(unsigned i=0;i<trust->revoked;i++)
  for(unsigned j=0;j<i;j++)if(!memcmp(trust->revoked_ids[i],trust->revoked_ids[j],32))return SF_BAD_FORMAT;

 uint8_t canonical[16+4+SF_TRUST_MAX*40+4+SF_TRUST_MAX*32];
 static const uint8_t domain[16]={'S','F','T','R','U','S','T','-','S','N','A','P','-','V','1',0};
 size_t used=0;memcpy(canonical+used,domain,sizeof(domain));used+=sizeof(domain);
 put32(canonical+used,policy->count);used+=4;
 unsigned order[SF_TRUST_MAX];
 for(unsigned i=0;i<policy->count;i++)order[i]=i;
 for(unsigned i=1;i<policy->count;i++){
  unsigned v=order[i],j=i;
  while(j&&key_less(policy->keys,v,order[j-1])){order[j]=order[j-1];j--;}
  order[j]=v;
 }
 for(unsigned i=0;i<policy->count;i++){
  const SfTrustKey*k=&policy->keys[order[i]];
  memcpy(canonical+used,k->public_key,32);used+=32;
  put32(canonical+used,k->usage);used+=4;put32(canonical+used,k->revoked);used+=4;
 }
 put32(canonical+used,trust->revoked);used+=4;
 unsigned ids[SF_TRUST_MAX];for(unsigned i=0;i<trust->revoked;i++)ids[i]=i;
 for(unsigned i=1;i<trust->revoked;i++){
  unsigned v=ids[i],j=i;
  while(j&&id_less(trust,v,ids[j-1])){ids[j]=ids[j-1];j--;}
  ids[j]=v;
 }
 for(unsigned i=0;i<trust->revoked;i++){
  memcpy(canonical+used,trust->revoked_ids[ids[i]],32);used+=32;
 }
 uint8_t digest[32];sf_sha256(digest,canonical,used);memcpy(out,digest,sizeof(digest));
 return SF_OK;
}
