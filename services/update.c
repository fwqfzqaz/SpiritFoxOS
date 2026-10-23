#include "libsf.h"
#include "update.h"
static int zero(const void*p,size_t n){const uint8_t*b=p;while(n--)if(*b++)return 0;return 1;}
static int slot(uint8_t s){return s=='a'||s=='b';}
static int identity(const char*s){
 unsigned n=0;for(;n<64&&s[n];n++){uint8_t c=s[n];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_'))return 0;}
 return n&&n<64&&zero(s+n,64-n);
}
static int valid(const SfUpdate*s){
 if(s->phase>SF_UPDATE_TRYING||(!slot(s->good)&&s->good)||(!slot(s->candidate)&&s->candidate))return 0;
 if((!s->good)!=zero(s->good_hash,32)||(!s->candidate)!=zero(s->candidate_hash,32))return 0;
 if(s->phase==SF_UPDATE_STABLE){if(s->candidate)return 0;}else if(!s->candidate||s->candidate==s->good)return 0;
 return s->good||s->candidate?identity(s->app_id):zero(s->app_id,64);
}
int sf_update_decode(SfUpdate*out,const void*data,size_t n){
 if(n!=SF_UPDATE_BYTES)return SF_BAD_FORMAT;const uint8_t*b=data;
 if(memcmp(b,"SFUPD1\0\0",8)||!zero(b+11,21))return SF_BAD_FORMAT;
 SfUpdate s={0};s.phase=b[8];s.good=b[9];s.candidate=b[10];memcpy(s.good_hash,b+32,32);memcpy(s.candidate_hash,b+64,32);memcpy(s.app_id,b+96,64);
 if(!valid(&s))return SF_BAD_FORMAT;*out=s;return 0;
}
int sf_update_encode(const SfUpdate*s,uint8_t out[SF_UPDATE_BYTES]){
 if(!valid(s))return SF_BAD_FORMAT;memset(out,0,SF_UPDATE_BYTES);memcpy(out,"SFUPD1\0\0",8);out[8]=s->phase;out[9]=s->good;out[10]=s->candidate;
 memcpy(out+32,s->good_hash,32);memcpy(out+64,s->candidate_hash,32);memcpy(out+96,s->app_id,64);return 0;
}
int sf_update_stage(SfUpdate*s,const char*id,const uint8_t hash[32]){
 if(!valid(s))return SF_BAD_FORMAT;if(s->phase!=SF_UPDATE_STABLE)return SF_BUSY;
 char name[64]={0};size_t n=strlen(id);if(n>=sizeof(name))return SF_BAD_FORMAT;memcpy(name,id,n);
 if(!identity(name)||zero(hash,32))return SF_BAD_FORMAT;if(s->good&&strcmp(s->app_id,name))return SF_DEPENDENCY;
 memcpy(s->app_id,name,64);s->candidate=s->good=='a'?'b':'a';memcpy(s->candidate_hash,hash,32);s->phase=SF_UPDATE_PENDING;return 0;
}
int sf_update_begin(SfUpdate*s){
 if(!valid(s))return SF_BAD_FORMAT;if(s->phase!=SF_UPDATE_PENDING)return SF_INVALID;s->phase=SF_UPDATE_TRYING;return 0;
}
int sf_update_finish(SfUpdate*s,int success){
 if(!valid(s))return SF_BAD_FORMAT;if(s->phase!=SF_UPDATE_TRYING)return SF_INVALID;
 if(success){s->good=s->candidate;memcpy(s->good_hash,s->candidate_hash,32);}
 s->phase=SF_UPDATE_STABLE;s->candidate=0;memset(s->candidate_hash,0,32);if(!s->good)memset(s->app_id,0,64);return 0;
}
