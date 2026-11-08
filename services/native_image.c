#include "native_image.h"
static uint64_t page_up(uint64_t v){return (v+SF_PAGE-1)&~(SF_PAGE-1);}
static int link_relocations(const SfPackage*p,uint64_t package_offset,uint64_t base,SfLoadPlan*plan,unsigned first,unsigned last){
 for(unsigned i=0;i<p->relocation_count;i++){
  const uint8_t*r=p->data+p->relocation_offset+i*16;uint64_t target=sf_u64(r)+package_offset,value=base+sf_u64(r+8)+package_offset;
  int wrote=0;
  for(unsigned j=first;j<last;j++){SfLoadSegment*s=&plan->segments[j];
   if(s->flags==3&&target>=s->rva&&target-s->rva<=s->bytes&&8<=s->bytes-(target-s->rva)){
    uint8_t*slot=(void*)(s->data+target-s->rva);for(unsigned k=0;k<8;k++)slot[k]=(uint8_t)(value>>(k*8));wrote=1;break;
   }
  }
  if(!wrote)return SF_BAD_FORMAT;
 }
 return 0;
}
static int link_import(const SfPackage*app,const SfPackage*library,uint64_t base,uint64_t lib_offset,SfLoadPlan*plan,unsigned app_last){
 for(unsigned i=0;i<app->import_count;i++){
  char name[SF_NATIVE_NAME_MAX],abi[SF_NATIVE_ABI_MAX];uint64_t slot,address=0;
  if(sf_native_import(app,i,name,abi,&slot))return SF_BAD_FORMAT;
  for(unsigned j=0;j<library->export_count;j++){
   char ename[SF_NATIVE_NAME_MAX],eabi[SF_NATIVE_ABI_MAX];uint64_t rva;
   if(sf_native_export(library,j,ename,eabi,&rva))return SF_BAD_FORMAT;
   if(!strcmp(name,ename)&&!strcmp(abi,eabi)){address=base+lib_offset+rva;break;}
  }
  if(!address)return SF_DEPENDENCY;
  int wrote=0;
  for(unsigned j=0;j<app_last;j++){SfLoadSegment*s=&plan->segments[j];
   if(s->flags==3&&slot>=s->rva&&slot-s->rva<=s->bytes&&8<=s->bytes-(slot-s->rva)){
    uint8_t*dst=(void*)(s->data+slot-s->rva);for(unsigned k=0;k<8;k++)dst[k]=(uint8_t)(address>>(k*8));wrote=1;break;
   }
  }
  if(!wrote)return SF_BAD_FORMAT;
 }
 return 0;
}
int sf_native_link(const SfPackage*app,const SfPackage*library,uint64_t base,void*buffer,size_t capacity,SfLoadPlan*plan){
 if(!app||!library||!buffer||!plan||!sf_image_base_valid(base)||base<SF_PIE_MIN)return SF_INVALID;
 if(app->native_profile!=SF_NATIVE_RELATIVE||library->native_profile!=SF_NATIVE_RELATIVE)return SF_BAD_FORMAT;
 if(app->kind!=2||!app->import_count||app->import_count>SF_NATIVE_IMPORT_MAX)return SF_BAD_FORMAT;
 if(library->kind!=1||library->native_role!=SF_NATIVE_ROLE_LIBRARY||!library->export_count||library->import_count)return SF_BAD_FORMAT;
 if(sf_native_relocations_validate(app)||sf_native_relocations_validate(library))return SF_BAD_FORMAT;
 uint64_t app_span=0,lib_span=0,app_used=0,lib_used=0;unsigned app_seg=0,lib_seg=0;
 for(unsigned i=0;i<app->count;i++){const SfSection*s=&app->sections[i];if(s->type>3)continue;uint64_t mem=page_up(s->memory);
  if(!s->memory||s->memory>SF_IMAGE_MAX||s->bytes>s->memory||s->offset>app->bytes||s->bytes>app->bytes-s->offset)return SF_BAD_FORMAT;
  if(s->rva+mem>SF_IMAGE_MAX)return SF_NO_SPACE;if(s->rva+mem>app_span)app_span=s->rva+mem;app_used+=mem;app_seg++;
 }
 for(unsigned i=0;i<library->count;i++){const SfSection*s=&library->sections[i];if(s->type>3)continue;uint64_t mem=page_up(s->memory);
  if(!s->memory||s->memory>SF_IMAGE_MAX||s->bytes>s->memory||s->offset>library->bytes||s->bytes>library->bytes-s->offset)return SF_BAD_FORMAT;
  if(s->rva+mem>lib_span)lib_span=s->rva+mem;lib_used+=mem;lib_seg++;
 }
 if(!app_seg||!lib_seg||app_seg+lib_seg>8)return SF_NO_SPACE;
 uint64_t lib_offset=page_up(app_span);
 if(!lib_offset||lib_offset>=SF_IMAGE_MAX||lib_span>SF_IMAGE_MAX-lib_offset)return SF_NO_SPACE;
 if(app_used>capacity||lib_used>capacity-app_used)return SF_NO_SPACE;
 plan->count=0;plan->entry=app->entry;plan->image_base=base;size_t used=0;
 for(unsigned i=0;i<app->count;i++){const SfSection*s=&app->sections[i];if(s->type>3)continue;size_t mem=page_up(s->memory);
  memset((uint8_t*)buffer+used,0,mem);memcpy((uint8_t*)buffer+used,app->data+s->offset,s->bytes);
  plan->segments[plan->count++]=(SfLoadSegment){s->rva,s->memory,(uint64_t)((uint8_t*)buffer+used),s->flags,0};used+=mem;
 }
 unsigned app_last=plan->count;
 for(unsigned i=0;i<library->count;i++){const SfSection*s=&library->sections[i];if(s->type>3)continue;size_t mem=page_up(s->memory);
  memset((uint8_t*)buffer+used,0,mem);memcpy((uint8_t*)buffer+used,library->data+s->offset,s->bytes);
  plan->segments[plan->count++]=(SfLoadSegment){lib_offset+s->rva,s->memory,(uint64_t)((uint8_t*)buffer+used),s->flags,0};used+=mem;
 }
 int r=link_relocations(app,0,base,plan,0,app_last);if(r)return r;
 r=link_relocations(library,lib_offset,base,plan,app_last,plan->count);if(r)return r;
 return link_import(app,library,base,lib_offset,plan,app_last);
}
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
