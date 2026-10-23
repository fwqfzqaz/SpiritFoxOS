#ifndef PACKAGE_H
#define PACKAGE_H
#include "sf.h"
#define SF_PACKAGE_MAX (32u*1024u*1024u)
typedef struct { uint32_t type,flags; uint64_t offset,bytes,memory,rva,alignment; } SfSection;
typedef struct {
 const uint8_t *data,*manifest,*key_id; uint64_t bytes,manifest_bytes,entry,signature;
 uint32_t count; uint16_t kind; SfSection sections[64];
} SfPackage;
/* Runtime trust policy: anchor keys plus revocation decisions made after boot.
 * The store is process-local state; persistence keeps it across restarts. */
#define SF_TRUST_MAX 4
typedef struct { unsigned count,revoked; uint8_t keys[SF_TRUST_MAX][32]; uint8_t revoked_ids[SF_TRUST_MAX][32]; } SfTrustStore;
extern SfTrustStore sf_trust;
uint16_t sf_u16(const uint8_t *);
uint32_t sf_u32(const uint8_t *);
uint64_t sf_u64(const uint8_t *);
void sf_sha256(uint8_t out[32],const uint8_t *data,size_t bytes);
int sf_package_open(SfPackage *,const void *,uint64_t,const uint8_t public_key[32]);
int sf_package_open_set(SfPackage *,const void *,uint64_t,const uint8_t (*keys)[32],unsigned count);
int sf_trust_init(SfTrustStore *,const uint8_t (*keys)[32],unsigned count);
int sf_trust_revoke(SfTrustStore *,const uint8_t key_id[32]);
int sf_trust_revoked(const SfTrustStore *,const uint8_t key_id[32]);
int sf_trust_open(SfTrustStore *,SfPackage *,const void *,uint64_t);
int sf_manifest_text(const SfPackage *,const char *key,char *out,size_t size);
int sf_manifest_capability(const SfPackage *,const char *);
int sf_bundle_find(const void *,uint64_t,const char *,const uint8_t **,uint64_t *);
#endif
