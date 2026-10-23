#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package.h"
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"TRUST FAIL line %d\n",__LINE__);exit(1);}}while(0)
int main(void){
 uint8_t anchor[SF_TRUST_MAX][32],id[SF_TRUST_MAX][32];
 for(unsigned i=0;i<SF_TRUST_MAX;i++)for(unsigned j=0;j<32;j++)anchor[i][j]=(uint8_t)(i*32+j);
 for(unsigned i=0;i<SF_TRUST_MAX;i++)sf_sha256(id[i],anchor[i],32);
 SfTrustStore store;
 /* Anchor validation happens before any state is touched. */
 CHECK(sf_trust_init(&store,anchor,0)==SF_INVALID);
 CHECK(sf_trust_init(&store,0,1)==SF_INVALID);
 CHECK(sf_trust_init(&store,anchor,SF_TRUST_MAX+1)==SF_INVALID);
 CHECK(!sf_trust_init(&store,anchor,SF_TRUST_MAX));
 CHECK(store.count==SF_TRUST_MAX&&!store.revoked);
 CHECK(!sf_trust_revoked(&store,id[0]));
 /* Revocation is idempotent; the cap is reported as SF_NO_SPACE. */
 for(unsigned i=0;i<SF_TRUST_MAX;i++){CHECK(!sf_trust_revoke(&store,id[i]));CHECK(sf_trust_revoked(&store,id[i]));}
 CHECK(sf_trust_revoke(&store,id[0])==0);
 uint8_t extra[32]={0xff};CHECK(sf_trust_revoke(&store,extra)==SF_NO_SPACE);
 CHECK(store.revoked==SF_TRUST_MAX);
 /* A fresh store is unaffected by revocations made in the old one. */
 CHECK(!sf_trust_init(&store,anchor,2));
 CHECK(!sf_trust_revoked(&store,id[0])&&!sf_trust_revoked(&store,extra));
 CHECK(sf_trust_revoke(&store,extra)==0&&sf_trust_revoked(&store,extra));
 /* Single-anchor stores are the boot-chain minimum. */
 CHECK(!sf_trust_init(&store,anchor,1)&&!sf_trust_revoked(&store,id[1]));
 printf("TRUST PASS: %u checks, anchor validation, idempotent revocation, capacity cap, store isolation\n",checks);return 0;
}
