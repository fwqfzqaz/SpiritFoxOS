#include "libsf.h"
#include "version.h"
int sf_version_parse(SfVersion*out,const char*text){
 SfVersion version={{0}};
 for(unsigned i=0;i<3;i++){
  const char*start=text;if(*text<'0'||*text>'9')return SF_BAD_FORMAT;
  do{uint32_t digit=*text-'0';if(version.part[i]>(UINT32_MAX-digit)/10)return SF_BAD_FORMAT;version.part[i]=version.part[i]*10+digit;text++;}while(*text>='0'&&*text<='9');
  if(text-start>1&&*start=='0')return SF_BAD_FORMAT;
  if(i<2){if(*text++!='.')return SF_BAD_FORMAT;}else if(*text)return SF_BAD_FORMAT;
 }
 *out=version;return 0;
}
int sf_version_compare(const SfVersion*a,const SfVersion*b){
 for(unsigned i=0;i<3;i++)if(a->part[i]!=b->part[i])return a->part[i]>b->part[i]?1:-1;return 0;
}
int sf_version_upgrade(const SfVersion*current,const uint8_t old[32],const SfVersion*candidate,const uint8_t next[32]){
 int r=sf_version_compare(candidate,current);if(r>0)return 0;if(r<0||memcmp(old,next,32))return SF_VERSION_REJECTED;return 1;
}
