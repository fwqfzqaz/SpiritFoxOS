#include "arguments.h"
static uint32_t get32(const uint8_t*p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int payload(const uint8_t*data,unsigned bytes,unsigned count){
 if(count>SF_ARGUMENT_MAX||bytes>SF_ARGUMENT_BYTES)return SF_BAD_FORMAT;
 if(!count)return bytes?SF_BAD_FORMAT:0;
 if(bytes<count*4)return SF_BAD_FORMAT;unsigned cursor=sizeof(SfBootArgs)+count*4,end=sizeof(SfBootArgs)+bytes;
 for(unsigned i=0;i<count;i++){
  if(get32(data+i*4)!=cursor)return SF_BAD_FORMAT;
  while(cursor<end&&data[cursor-sizeof(SfBootArgs)])cursor++;
  if(cursor==end)return SF_BAD_FORMAT;cursor++;
 }return cursor==end?0:SF_BAD_FORMAT;
}
int sf_arguments_validate(const SfArguments*a){return a?payload(a->data,a->bytes,a->count):SF_INVALID;}
int sf_boot_arguments_validate(const SfBootArgs*a,size_t bytes){
 if(!a||bytes<sizeof(*a)||bytes>sizeof(*a)+SF_ARGUMENT_BYTES)return SF_BAD_FORMAT;
 if(a->size!=bytes||a->abi_major!=1||a->abi_minor)return SF_ABI_MISMATCH;
 if(a->envc||a->env_offset||a->argv_offset!=(a->argc?sizeof(*a):0))return SF_BAD_FORMAT;
 return payload((const uint8_t*)a+sizeof(*a),bytes-sizeof(*a),a->argc);
}
const char*sf_argument(const SfBootArgs*a,size_t bytes,unsigned index){
 if(sf_boot_arguments_validate(a,bytes)||index>=a->argc)return 0;
 return(const char*)a+get32((const uint8_t*)a+a->argv_offset+index*4);
}
