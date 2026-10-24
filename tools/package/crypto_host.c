#include "package.h"
#include "native_image.h"
#include "monocypher-ed25519.h"
#include "readfs.h"
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
EXPORT void sf_key_pair(uint8_t secret[64],uint8_t pub[32],uint8_t seed[32]){crypto_ed25519_key_pair(secret,pub,seed);}
EXPORT void sf_sign(uint8_t sig[64],const uint8_t secret[64],const uint8_t*data,size_t n){crypto_ed25519_sign(sig,secret,data,n);}
EXPORT int sf_check(const uint8_t*data,size_t n,const uint8_t pub[32]){SfPackage p;return sf_package_open(&p,data,n,pub);}
EXPORT int sf_trust_check(const uint8_t*data,size_t n,const SfTrustKey*keys,uint32_t count){SfPackage p;SfTrustStore store={keys,count};return sf_package_open_trusted(&p,data,n,&store);}
EXPORT int sf_trust_policy_check(const SfTrustKey*keys,uint32_t count){SfTrustStore store={keys,count};return sf_trust_validate(&store);}
EXPORT int sf_runtime_policy_check(const uint8_t*data,size_t n,const SfTrustKey*keys,uint32_t count,const uint8_t ids[][32],unsigned revoked){
 SfTrustStore policy={keys,count};SfRuntimeTrust s;SfPackage p;int r=sf_trust_init(&s,&policy);if(r)return r;
 if(revoked>SF_TRUST_MAX||(!ids&&revoked))return SF_INVALID;
 for(unsigned i=0;i<revoked;i++)if((r=sf_trust_revoke(&s,ids[i])))return r;
 return sf_trust_open(&s,&p,data,n);
}
EXPORT int sf_check_set(const uint8_t*data,size_t n,const uint8_t keys[][32],unsigned count){SfPackage p;return sf_package_open_set(&p,data,n,keys,count);}
static int raw_policy(SfTrustKey*entries,SfTrustStore*policy,const uint8_t keys[][32],unsigned count){
 if(!keys||!count||count>SF_TRUST_MAX)return SF_INVALID;
 memset(entries,0,sizeof(SfTrustKey)*SF_TRUST_MAX);
 for(unsigned i=0;i<count;i++){memcpy(entries[i].public_key,keys[i],32);entries[i].usage=SF_TRUST_MODULE|SF_TRUST_APP;}
 policy->keys=entries;policy->count=count;return 0;
}
EXPORT int sf_runtime_trust_check(const uint8_t*data,size_t n,const uint8_t keys[][32],unsigned count,const uint8_t ids[][32],unsigned revoked){
 SfTrustKey entries[SF_TRUST_MAX];SfTrustStore policy;SfRuntimeTrust s;SfPackage p;
 int r=raw_policy(entries,&policy,keys,count);if(r)return r;r=sf_trust_init(&s,&policy);if(r)return r;
 if(revoked>SF_TRUST_MAX||(!ids&&revoked))return SF_INVALID;
 for(unsigned i=0;i<revoked;i++)if((r=sf_trust_revoke(&s,ids[i])))return r;
 return sf_trust_open(&s,&p,data,n);
}
EXPORT int sf_trust_revoke_id(const uint8_t keys[][32],unsigned count,const uint8_t key_id[32]){
 SfTrustKey entries[SF_TRUST_MAX];SfTrustStore policy;SfRuntimeTrust s;int r=raw_policy(entries,&policy,keys,count);if(r)return r;
 r=sf_trust_init(&s,&policy);return r?r:sf_trust_revoke(&s,key_id);
}
EXPORT int sf_cap_check(const uint8_t*data,size_t n,const uint8_t pub[32],const char*cap){SfPackage p;int r=sf_package_open(&p,data,n,pub);return r?r:sf_manifest_capability(&p,cap);}
EXPORT int sf_ed_check(const uint8_t sig[64],const uint8_t pub[32],const uint8_t*data,size_t n){return crypto_ed25519_check(sig,pub,data,n);}
EXPORT void sf_hash(uint8_t hash[32],const uint8_t*data,size_t n){sf_sha256(hash,data,n);}
typedef struct {const uint8_t*data;size_t bytes;} MemoryDisk;
static int memory_sector(void*context,uint64_t sector,uint8_t out[512]){MemoryDisk*d=context;if(sector>=d->bytes/512)return SF_INVALID;memcpy(out,d->data+sector*512,512);return 0;}
EXPORT int sf_fs_check(const uint8_t*data,size_t bytes,int contents){MemoryDisk disk={data,bytes};SfReadFs fs;int r=sf_readfs_mount(&fs,memory_sector,&disk,bytes);if(r||!contents)return r;uint8_t buffer[SF_READFS_FILE_MAX];for(unsigned i=0;i<fs.count;i++){long n=sf_readfs_load(&fs,i,buffer,sizeof(buffer));if(n<0)return n;}return 0;}

EXPORT int sf_image_prepare(const uint8_t*data,size_t n,const uint8_t pub[32],uint64_t base,uint8_t*image,size_t capacity){SfPackage p;int r=sf_package_open(&p,data,n,pub);if(r)return r;SfLoadPlan plan={0};r=sf_native_image(&p,base,image,capacity,&plan);if(r)return r;uint64_t bytes=0;for(unsigned i=0;i<plan.count;i++)bytes+=(plan.segments[i].bytes+4095)&~4095ull;return (int)bytes;}
