#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "version.h"
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"VERSION FAIL line %d\n",__LINE__);exit(1);}}while(0)
int main(void){
 const char*valid[]={"0.0.0","0.1.0","1.0.0","1.2.10","4294967295.0.0","4294967295.4294967295.4294967295"};
 const char*invalid[]={"","0","0.1","0.1.2.3",".1.2","1..2","1.2.","01.2.3","1.02.3","1.2.03","-1.2.3","+1.2.3","1.2.3 "," 1.2.3","1.2.3-alpha","1.2.3+build","4294967296.0.0","0.4294967296.0","0.0.4294967296","99999999999999999999.0.0","1.a.0","1.2.\xff"};
 SfVersion version={{99,98,97}},before=version;
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){CHECK(sf_version_parse(&version,invalid[i])==SF_BAD_FORMAT);CHECK(!memcmp(&version,&before,sizeof(version)));}
 for(unsigned i=0;i<sizeof(valid)/sizeof(*valid);i++){
  CHECK(!sf_version_parse(&version,valid[i]));char text[33];snprintf(text,sizeof(text),"%u.%u.%u",version.part[0],version.part[1],version.part[2]);CHECK(!strcmp(text,valid[i]));
 }
 SfVersion low,high;CHECK(!sf_version_parse(&low,"1.2.9"));CHECK(!sf_version_parse(&high,"1.2.10"));CHECK(sf_version_compare(&low,&high)==-1&&sf_version_compare(&high,&low)==1);
 CHECK(!sf_version_parse(&low,"1.4294967295.4294967295"));CHECK(!sf_version_parse(&high,"2.0.0"));CHECK(sf_version_compare(&high,&low)==1);
 CHECK(!sf_version_parse(&low,"2.9.4294967295"));CHECK(!sf_version_parse(&high,"2.10.0"));CHECK(sf_version_compare(&high,&low)==1);
 uint8_t hash[32]={1},other[32]={2};
 CHECK(sf_version_upgrade(&low,hash,&high,other)==0);CHECK(sf_version_upgrade(&high,hash,&low,other)==SF_VERSION_REJECTED);
 CHECK(sf_version_upgrade(&high,hash,&high,other)==SF_VERSION_REJECTED);CHECK(sf_version_upgrade(&high,hash,&high,hash)==1);
 uint32_t random=20261023;
 for(unsigned i=0;i<1000;i++){
  SfVersion original;for(unsigned j=0;j<3;j++){random=random*1664525u+1013904223u;original.part[j]=random;}
  char text[33];snprintf(text,sizeof(text),"%u.%u.%u",original.part[0],original.part[1],original.part[2]);CHECK(!sf_version_parse(&version,text));CHECK(!memcmp(&version,&original,sizeof(version)));
  SfVersion next=version;if(next.part[2]!=UINT32_MAX){next.part[2]++;CHECK(sf_version_upgrade(&version,hash,&next,hash)==0);CHECK(sf_version_upgrade(&next,hash,&version,hash)==SF_VERSION_REJECTED);}
 }
 printf("VERSION PASS: %u checks, canonical numeric versions, overflow and monotonic digest-bound policy\n",checks);return 0;
}
