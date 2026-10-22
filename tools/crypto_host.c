#include "package.h"
#include "monocypher-ed25519.h"
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
