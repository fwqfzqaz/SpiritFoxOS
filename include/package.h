#ifndef PACKAGE_H
#define PACKAGE_H
#include "sf.h"
#define SF_PACKAGE_MAX (32u*1024u*1024u)
typedef struct { uint32_t type,flags; uint64_t offset,bytes,memory,rva,alignment; } SfSection;
typedef struct {
 const uint8_t *data,*manifest; uint64_t bytes,manifest_bytes,entry,signature;
 uint32_t count; uint16_t kind; SfSection sections[64];
} SfPackage;
uint16_t sf_u16(const uint8_t *);
uint32_t sf_u32(const uint8_t *);
uint64_t sf_u64(const uint8_t *);
void sf_sha256(uint8_t out[32],const uint8_t *data,size_t bytes);
int sf_package_open(SfPackage *,const void *,uint64_t,const uint8_t public_key[32]);
int sf_manifest_text(const SfPackage *,const char *key,char *out,size_t size);
int sf_bundle_find(const void *,uint64_t,const char *,const uint8_t **,uint64_t *);
#endif
