#include "package.h"
/* Firmware and native retain immutable policy; the manager additionally checks
 * durable runtime revocations before dispatching any package. */
SfRuntimeTrust sf_trust;
int sf_trust_init(SfRuntimeTrust*s,const SfTrustStore*policy){
 if(!s||sf_trust_validate(policy))return SF_INVALID;
 memset(s,0,sizeof(*s));s->policy=*policy;return 0;
}
int sf_trust_revoked(const SfRuntimeTrust*s,const uint8_t key_id[32]){
 if(!s||!key_id)return 0;
 for(unsigned i=0;i<s->revoked;i++)if(!memcmp(s->revoked_ids[i],key_id,32))return 1;return 0;
}
int sf_trust_revoke(SfRuntimeTrust*s,const uint8_t key_id[32]){
 if(!s||!key_id)return SF_INVALID;
 if(sf_trust_revoked(s,key_id))return 0;
 if(s->revoked>=SF_TRUST_MAX)return SF_NO_SPACE;
 memcpy(s->revoked_ids[s->revoked++],key_id,32);return 0;
}
int sf_trust_open(const SfRuntimeTrust*s,SfPackage*p,const void*data,uint64_t bytes){
 if(!s)return SF_INVALID;
 int r=sf_package_open_trusted(p,data,bytes,&s->policy);if(r)return r;
 return sf_trust_revoked(s,p->key_id)?SF_REVOKED:0;
}
