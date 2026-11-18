#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "../../services/trust_fingerprint.h"

static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"trust fingerprint failure line %d: %s\n",__LINE__,#c);exit(1);}}while(0)

static void fixture(SfRuntimeTrust*t,SfTrustKey keys[SF_TRUST_MAX]){
 memset(keys,0xa7,sizeof(SfTrustKey)*SF_TRUST_MAX);
 for(unsigned i=0;i<3;i++){
  for(unsigned j=0;j<32;j++)keys[i].public_key[j]=(uint8_t)(i*41u+j*3u);
  keys[i].usage=i==0?SF_TRUST_MODULE:i==1?SF_TRUST_APP:(SF_TRUST_MODULE|SF_TRUST_APP);
  keys[i].revoked=i==1;
 }
 memset(t,0x5c,sizeof(*t));t->policy.keys=keys;t->policy.count=3;t->revoked=2;
 for(unsigned i=0;i<32;i++){t->revoked_ids[0][i]=(uint8_t)(0xe0u-i);t->revoked_ids[1][i]=(uint8_t)(0x20u+i*2u);}
}
static void digest(const SfRuntimeTrust*t,uint8_t out[32]){CHECK(sf_trust_fingerprint(t,out)==SF_OK);}
static void rejects_untouched(const SfRuntimeTrust*t,uint8_t*out,int expected){
 uint8_t before[32];memcpy(before,out,32);
 int r=sf_trust_fingerprint(t,out);CHECK(r<0);if(expected)CHECK(r==expected);CHECK(!memcmp(out,before,32));
}
static int hex_digit(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;}

static void canonical_tests(const char*expected_hex){
 SfRuntimeTrust t,reordered,unused;SfTrustKey keys[SF_TRUST_MAX],reverse[SF_TRUST_MAX];
 fixture(&t,keys);uint8_t a[32],b[32];digest(&t,a);
 memcpy(reverse,keys,sizeof(keys));
 for(unsigned i=0;i<3;i++)reverse[i]=keys[2-i];
 reordered=t;reordered.policy.keys=reverse;
 memcpy(reordered.revoked_ids[0],t.revoked_ids[1],32);
 memcpy(reordered.revoked_ids[1],t.revoked_ids[0],32);
 digest(&reordered,b);CHECK(!memcmp(a,b,32));
 memcpy(reverse,keys,sizeof(keys));unused=t;unused.policy.keys=reverse;
 for(unsigned i=3;i<SF_TRUST_MAX;i++)memset(reverse[i].public_key,(int)(0x30+i),32);
 for(unsigned i=unused.revoked;i<SF_TRUST_MAX;i++)memset(unused.revoked_ids[i],(int)(0x90+i),32);
 digest(&unused,b);CHECK(!memcmp(a,b,32));
 if(expected_hex){
  CHECK(strlen(expected_hex)==64);
  uint8_t expected[32];for(unsigned i=0;i<32;i++){
   int hi=hex_digit(expected_hex[i*2]),lo=hex_digit(expected_hex[i*2+1]);CHECK(hi>=0&&lo>=0);
   expected[i]=(uint8_t)((hi<<4)|lo);
  }
  CHECK(!memcmp(a,expected,32));
 }
}
static void semantic_mutations(void){
 SfRuntimeTrust base,copy;SfTrustKey keys[SF_TRUST_MAX],changed[SF_TRUST_MAX];uint8_t expected[32],actual[32];
 fixture(&base,keys);digest(&base,expected);
 for(unsigned mode=0;mode<5;mode++){
  copy=base;memcpy(changed,keys,sizeof(keys));copy.policy.keys=changed;
  switch(mode){
   case 0:changed[0].usage=SF_TRUST_APP;break;
   case 1:changed[0].revoked=1;break;
   case 2:changed[0].public_key[0]^=0x80;break;
   case 3:copy.revoked_ids[0][0]^=1;break;
   case 4:copy.revoked=1;break;
  }
  digest(&copy,actual);CHECK(memcmp(expected,actual,32)!=0);
 }
}
static void invalid_inputs(void){
 SfRuntimeTrust t;SfTrustKey keys[SF_TRUST_MAX];uint8_t out[32];fixture(&t,keys);memset(out,0x42,sizeof(out));
 CHECK(sf_trust_fingerprint(0,out)==SF_INVALID);CHECK(out[0]==0x42);
 CHECK(sf_trust_fingerprint(&t,0)==SF_INVALID);
 SfRuntimeTrust bad=t;bad.policy.count=0;rejects_untouched(&bad,out,SF_BAD_FORMAT);
 bad=t;bad.policy.keys=0;rejects_untouched(&bad,out,SF_BAD_FORMAT);
 bad=t;bad.policy.count=SF_TRUST_MAX+1;rejects_untouched(&bad,out,SF_BAD_FORMAT);
 bad=t;bad.revoked=SF_TRUST_MAX+1;rejects_untouched(&bad,out,SF_BAD_FORMAT);
 SfTrustKey invalid[SF_TRUST_MAX];memcpy(invalid,keys,sizeof(keys));bad=t;bad.policy.keys=invalid;
 invalid[0].usage=0;rejects_untouched(&bad,out,SF_BAD_FORMAT);
 memcpy(invalid,keys,sizeof(keys));invalid[0].usage=SF_TRUST_MODULE|SF_TRUST_APP|4;rejects_untouched(&bad,out,SF_BAD_FORMAT);
 memcpy(invalid,keys,sizeof(keys));invalid[0].revoked=2;rejects_untouched(&bad,out,SF_BAD_FORMAT);
 memcpy(invalid,keys,sizeof(keys));memcpy(invalid[2].public_key,invalid[1].public_key,32);rejects_untouched(&bad,out,SF_BAD_FORMAT);
 bad=t;memcpy(bad.revoked_ids[1],bad.revoked_ids[0],32);rejects_untouched(&bad,out,SF_BAD_FORMAT);
}
static void alias_and_overflow(void){
 SfRuntimeTrust t;SfTrustKey keys[SF_TRUST_MAX];fixture(&t,keys);uint8_t out[32];
 SfRuntimeTrust saved=t;rejects_untouched(&t,(uint8_t*)&t+offsetof(SfRuntimeTrust,revoked_ids),SF_INVALID);
 CHECK(!memcmp(&t,&saved,sizeof(t)));
 SfTrustKey key_saved[SF_TRUST_MAX];memcpy(key_saved,keys,sizeof(keys));
 rejects_untouched(&t,keys[0].public_key,SF_INVALID);CHECK(!memcmp(keys,key_saved,sizeof(keys)));
 memset(out,0x6d,sizeof(out));
 const SfRuntimeTrust*bad_trust=(const SfRuntimeTrust*)(uintptr_t)(UINTPTR_MAX-4u);
 rejects_untouched(bad_trust,out,SF_INVALID);
 SfRuntimeTrust bad=t;bad.policy.keys=(const SfTrustKey*)(uintptr_t)(UINTPTR_MAX-8u);
 rejects_untouched(&bad,out,SF_INVALID);
 uint8_t*bad_out=(uint8_t*)(uintptr_t)(UINTPTR_MAX-8u);
 CHECK(sf_trust_fingerprint(&t,bad_out)==SF_INVALID);
}
int main(int argc,char**argv){
 canonical_tests(argc>1?argv[1]:0);semantic_mutations();invalid_inputs();alias_and_overflow();
 printf("TRUST FINGERPRINT PASS %u checks; E1 helper host fixture; canonical expected digest %s\n",checks,argc>1?"provided":"external-vector pending");
 return 0;
}
