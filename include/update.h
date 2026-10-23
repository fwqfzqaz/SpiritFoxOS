#ifndef SF_UPDATE_H
#define SF_UPDATE_H
#include "sf.h"
#define SF_UPDATE_BYTES 160u
enum { SF_UPDATE_STABLE, SF_UPDATE_PENDING, SF_UPDATE_TRYING };
typedef struct {
 uint8_t phase,good,candidate;
 uint8_t good_hash[32],candidate_hash[32];
 char app_id[64];
} SfUpdate;
int sf_update_decode(SfUpdate*,const void*,size_t);
int sf_update_encode(const SfUpdate*,uint8_t out[SF_UPDATE_BYTES]);
int sf_update_stage(SfUpdate*,const char*,const uint8_t hash[32]);
int sf_update_begin(SfUpdate*);
int sf_update_finish(SfUpdate*,int success);
#endif
