#include "package.h"
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
EXPORT int sf_ed_check(const uint8_t sig[64],const uint8_t pub[32],const uint8_t*data,size_t n){return crypto_ed25519_check(sig,pub,data,n);}
EXPORT void sf_hash(uint8_t hash[32],const uint8_t*data,size_t n){sf_sha256(hash,data,n);}
typedef struct {const uint8_t*data;size_t bytes;} MemoryDisk;
static int memory_sector(void*context,uint64_t sector,uint8_t out[512]){MemoryDisk*d=context;if(sector>=d->bytes/512)return SF_INVALID;memcpy(out,d->data+sector*512,512);return 0;}
EXPORT int sf_fs_check(const uint8_t*data,size_t bytes,int contents){MemoryDisk disk={data,bytes};SfReadFs fs;int r=sf_readfs_mount(&fs,memory_sector,&disk,bytes);if(r||!contents)return r;uint8_t buffer[SF_READFS_FILE_MAX];for(unsigned i=0;i<fs.count;i++){long n=sf_readfs_load(&fs,i,buffer,sizeof(buffer));if(n<0)return n;}return 0;}
