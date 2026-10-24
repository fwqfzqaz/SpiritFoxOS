#include "arguments.h"
static uint32_t get32(const uint8_t*p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int payload(const uint8_t*data,unsigned bytes,unsigned count){
 if(count>SF_ARGUMENT_MAX+SF_ENVIRONMENT_MAX||bytes>SF_ARGUMENT_BYTES)return SF_BAD_FORMAT;
 if(!count)return bytes?SF_BAD_FORMAT:0;
 if(bytes<count*4)return SF_BAD_FORMAT;unsigned cursor=sizeof(SfBootArgs)+count*4,end=sizeof(SfBootArgs)+bytes;
 for(unsigned i=0;i<count;i++){
  if(get32(data+i*4)!=cursor)return SF_BAD_FORMAT;
  while(cursor<end&&data[cursor-sizeof(SfBootArgs)])cursor++;
  if(cursor==end)return SF_BAD_FORMAT;cursor++;
 }return cursor==end?0:SF_BAD_FORMAT;
}
int sf_arguments_validate(const SfArguments*a){
 if(!a)return SF_INVALID;if(a->reserved||a->count>SF_ARGUMENT_MAX||a->env_count>SF_ENVIRONMENT_MAX)return SF_BAD_FORMAT;
 return payload(a->data,a->bytes,a->count+a->env_count);
}
int sf_environment_name_valid(const char*s,size_t n){
 if(!s||!n||n>63)return 0;for(size_t i=0;i<n;i++){unsigned c=(uint8_t)s[i];if(c=='_'||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||(i&&c>='0'&&c<='9'))continue;return 0;}return 1;
}
static unsigned key_length(const char*s){unsigned n=0;while(n<64&&s[n]&&s[n]!='=')n++;return n<64&&s[n]=='='?n:0;}
int sf_boot_arguments_validate(const SfBootArgs*a,size_t bytes){
 if(!a||bytes<sizeof(*a)||bytes>sizeof(*a)+SF_ARGUMENT_BYTES)return SF_BAD_FORMAT;
 if(a->size!=bytes||a->abi_major!=1||a->abi_minor)return SF_ABI_MISMATCH;
 if(a->argc>SF_ARGUMENT_MAX||a->envc>SF_ENVIRONMENT_MAX||a->argv_offset!=(a->argc?sizeof(*a):0)||a->env_offset!=(a->envc?sizeof(*a)+a->argc*4:0))return SF_BAD_FORMAT;
 int r=payload((const uint8_t*)a+sizeof(*a),bytes-sizeof(*a),a->argc+a->envc);if(r)return r;
 for(unsigned i=0;i<a->envc;i++){const char*s=(const char*)a+get32((const uint8_t*)a+a->env_offset+i*4);unsigned n=key_length(s);if(!sf_environment_name_valid(s,n))return SF_BAD_FORMAT;
  for(unsigned j=0;j<i;j++){const char*other=(const char*)a+get32((const uint8_t*)a+a->env_offset+j*4);if(key_length(other)==n&&!memcmp(s,other,n))return SF_BAD_FORMAT;}
 }return 0;
}
const char*sf_environment(const SfBootArgs*a,size_t bytes,unsigned index){
 if(sf_boot_arguments_validate(a,bytes)||index>=a->envc)return 0;
 return(const char*)a+get32((const uint8_t*)a+a->env_offset+index*4);
}
const char*sf_environment_value(const SfBootArgs*a,size_t bytes,const char*name){
 if(!name||sf_boot_arguments_validate(a,bytes))return 0;unsigned n=0;while(n<64&&name[n])n++;if(!sf_environment_name_valid(name,n))return 0;
 for(unsigned i=0;i<a->envc;i++){const char*s=(const char*)a+get32((const uint8_t*)a+a->env_offset+i*4);if(key_length(s)==n&&!memcmp(s,name,n))return s+n+1;}return 0;
}
const char*sf_argument(const SfBootArgs*a,size_t bytes,unsigned index){
 if(sf_boot_arguments_validate(a,bytes)||index>=a->argc)return 0;
 return(const char*)a+get32((const uint8_t*)a+a->argv_offset+index*4);
}
