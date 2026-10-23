#include "package.h"
/* Anchor keys come from the build-time trust header; revocation is runtime
 * policy loaded from the transaction volume. Boot-chain packages verified
 * before that state is readable can only be constrained by the anchors. */
SfTrustStore sf_trust;
int sf_trust_init(SfTrustStore*s,const uint8_t (*keys)[32],unsigned count){
 if(!count||count>SF_TRUST_MAX||!keys)return SF_INVALID;memset(s,0,sizeof(*s));s->count=count;
 for(unsigned i=0;i<count;i++)memcpy(s->keys[i],keys[i],32);return 0;
}
int sf_trust_revoked(const SfTrustStore*s,const uint8_t key_id[32]){
 for(unsigned i=0;i<s->revoked;i++)if(!memcmp(s->revoked_ids[i],key_id,32))return 1;return 0;
}
int sf_trust_revoke(SfTrustStore*s,const uint8_t key_id[32]){
 if(sf_trust_revoked(s,key_id))return 0;
 if(s->revoked>=SF_TRUST_MAX)return SF_NO_SPACE;
 memcpy(s->revoked_ids[s->revoked++],key_id,32);return 0;
}
int sf_trust_open(SfTrustStore*s,SfPackage*p,const void*data,uint64_t bytes){
 int r=sf_package_open_set(p,data,bytes,s->keys,s->count);if(r)return r;
 /* A valid signature from a revoked signer is a policy failure, distinct
  * from SF_BAD_SIGNATURE so diagnostics name the actual cause. */
 return sf_trust_revoked(s,p->key_id)?SF_REVOKED:0;
}
