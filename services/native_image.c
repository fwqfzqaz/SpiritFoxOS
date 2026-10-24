#include "native_image.h"
int sf_native_image(const SfPackage*p,uint64_t base,void*buffer,size_t capacity,SfLoadPlan*plan){
 if(!p||!buffer||!plan||!sf_image_base_valid(base)||(!base&&p->native_profile))return SF_INVALID;
 if(!base)base=SF_BASE;
 if(p->native_profile==SF_NATIVE_RELATIVE&&base<SF_PIE_MIN)return SF_INVALID;
 if((p->native_profile==SF_NATIVE_STATIC&&base!=SF_BASE)||sf_native_relocations_validate(p))return SF_BAD_FORMAT;
 size_t used=0;unsigned count=0;
 /* Preflight all allocations before exposing a partially prepared plan. */
 for(unsigned i=0;i<p->count;i++){const SfSection*s=&p->sections[i];if(s->type>3)continue;
  uint64_t mem=(s->memory+4095)&~4095ull;
  if(!s->memory||s->memory>SF_IMAGE_MAX||s->bytes>s->memory||s->offset>p->bytes||s->bytes>p->bytes-s->offset||mem>capacity-used||count==8)return SF_NO_SPACE;
  used+=mem;count++;
 }
 if(!count)return SF_BAD_FORMAT;
 used=0;plan->count=0;plan->entry=p->entry;plan->image_base=base;
 for(unsigned i=0;i<p->count;i++){const SfSection*s=&p->sections[i];if(s->type>3)continue;size_t mem=(s->memory+4095)&~4095ull;
  memset((uint8_t*)buffer+used,0,mem);memcpy((uint8_t*)buffer+used,p->data+s->offset,s->bytes);
  plan->segments[plan->count++]=(SfLoadSegment){s->rva,s->memory,(uint64_t)((uint8_t*)buffer+used),s->flags,0};used+=mem;
 }
 for(unsigned i=0;i<p->relocation_count;i++){
  const uint8_t*r=p->data+p->relocation_offset+i*16;uint64_t target=sf_u64(r),value=base+sf_u64(r+8);
  for(unsigned j=0;j<plan->count;j++){SfLoadSegment*s=&plan->segments[j];if(s->flags==3&&target>=s->rva&&target-s->rva<=s->bytes&&8<=s->bytes-(target-s->rva)){
   uint8_t*slot=(void*)(s->data+target-s->rva);for(unsigned k=0;k<8;k++)slot[k]=(uint8_t)(value>>(k*8));break;
  }}
 }
 return 0;
}
