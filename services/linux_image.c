#include "linux_image.h"
#define ELF_HEADER 64u
#define ELF_PHENT 56u
#define ET_DYN 3u
#define EM_X86_64 62u
#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PT_TLS 7u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u
#define DT_NULL 0u
#define DT_NEEDED 1u
#define DT_RELA 7u
#define DT_RELASZ 8u
#define DT_RELAENT 9u
#define DT_REL 17u
#define DT_RELSZ 18u
#define DT_TEXTREL 22u
#define DT_FLAGS 30u
#define DT_RELRSZ 35u
#define DT_RELR 36u
#define DF_TEXTREL 4u
#define R_X86_64_RELATIVE 8u
#define PLAN_READ 1u
#define PLAN_WRITE 3u
#define PLAN_EXEC 5u
typedef struct { uint64_t offset,file,va,mem,page,end; uint32_t flags; } LinuxRaw;
typedef struct { uint64_t start,end; uint32_t flags; } LinuxMerged;
typedef struct {
 SfLinuxInfo info;
 LinuxRaw raw[SF_LINUX_RAW_SEGMENT_MAX];
 LinuxMerged merged[SF_LINUX_SEGMENT_MAX];
 unsigned raw_count,merged_count;
 uint64_t dynamic_offset,dynamic_bytes,rela_offset,rela_bytes;
 uint32_t rela_count;
} LinuxElf;
static uint16_t le16(const uint8_t*p){return(uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t le32(const uint8_t*p){return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t*p){return(uint64_t)le32(p)|(uint64_t)le32(p+4)<<32;}
static void put64(uint8_t*p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8));}
static uint64_t page_up(uint64_t v){return(v+SF_PAGE-1)&~(SF_PAGE-1);}
static uint32_t plan_flags(uint32_t elf){
 if(elf==(PF_R|PF_X))return PLAN_EXEC;
 if(elf==(PF_R|PF_W))return PLAN_WRITE;
 return PLAN_READ;
}
static int vaddr_file(const LinuxElf*e,uint64_t va,uint64_t len,uint64_t*out){
 for(unsigned i=0;i<e->raw_count;i++){const LinuxRaw*s=&e->raw[i];
  if(va<s->va||va-s->va>=s->file||len>s->file-(va-s->va))continue;
  *out=s->offset+(va-s->va);return 1;
 }
 return 0;
}
static int bounded_length(const char*s,size_t limit,size_t*out){
 if(!s||!out)return SF_BAD_FORMAT;
 for(size_t i=0;i<limit;i++)if(!s[i]){*out=i;return SF_OK;}
 return SF_BAD_FORMAT;
}
static int decode(const uint8_t*image,size_t bytes,LinuxElf*e){
 if(!image||!e||bytes<ELF_HEADER)return SF_BAD_FORMAT;
 if(memcmp(image,"\x7f""ELF",4)||image[4]!=2||image[5]!=1||image[6]!=1)return SF_BAD_FORMAT;
 if(le16(image+16)!=ET_DYN||le16(image+18)!=EM_X86_64||le32(image+20)!=1)return SF_BAD_FORMAT;
 if(le16(image+52)!=ELF_HEADER||le16(image+54)!=ELF_PHENT||le16(image+58)!=ELF_HEADER)return SF_BAD_FORMAT;
 uint64_t phoff=le64(image+32),phnum=le16(image+56),entry=le64(image+24);
 if(!phnum||phnum>SF_LINUX_RAW_SEGMENT_MAX)return SF_BAD_FORMAT;
 if(phoff>bytes||phnum*ELF_PHENT>bytes-phoff)return SF_BAD_FORMAT;
 if(entry>=SF_IMAGE_MAX)return SF_BAD_FORMAT;
 memset(e,0,sizeof(*e));
 for(uint64_t i=0;i<phnum;i++){
  const uint8_t*p=image+phoff+i*ELF_PHENT;
  uint32_t type=le32(p),flags=le32(p+4);
  uint64_t offset=le64(p+8),va=le64(p+16),file=le64(p+32),mem=le64(p+40),align=le64(p+48);
  if(type==PT_INTERP||type==PT_TLS)return SF_DEPENDENCY;
  if(type==PT_DYNAMIC){
   if(!mem||offset>bytes||mem>bytes-offset)return SF_BAD_FORMAT;
   e->dynamic_offset=offset;e->dynamic_bytes=mem;continue;
  }
  if(type!=PT_LOAD)continue;
  if(!mem||file>mem)return SF_BAD_FORMAT;
  if(flags!=PF_R&&flags!=(PF_R|PF_W)&&flags!=(PF_R|PF_X))return SF_BAD_FORMAT;
  if(mem>SF_IMAGE_MAX||va>=SF_IMAGE_MAX||mem>SF_IMAGE_MAX-va)return SF_NO_SPACE;
  if(align&&((align&(align-1))||align>0x200000||(va%align)!=(offset%align)))return SF_BAD_FORMAT;
  /* 段内偏移必须与虚拟地址同余，才能用整页映射表达；真实链接器常有非页对齐
   * 的 p_vaddr（如 0x12a0/0x2a0），此时页基址向前取整。 */
  uint64_t delta=va&(SF_PAGE-1);
  if((offset&(SF_PAGE-1))!=delta)return SF_BAD_FORMAT;
  if(offset>bytes||file>bytes-offset)return SF_BAD_FORMAT;
  LinuxRaw*s=&e->raw[e->raw_count++];
  s->offset=offset;s->file=file;s->va=va;s->mem=mem;s->flags=flags;
  s->page=va-delta;s->end=page_up(va+mem);
 }
 if(!e->raw_count)return SF_BAD_FORMAT;
 /* Sort by page and merge only adjacent same-permission segments. Any page
  * overlap, including equal-permission aliases, is rejected. */
 for(unsigned i=1;i<e->raw_count;i++){
  LinuxRaw t=e->raw[i];unsigned j=i;
  while(j&&e->raw[j-1].page>t.page){e->raw[j]=e->raw[j-1];j--;}
  e->raw[j]=t;
 }
 for(unsigned i=0;i<e->raw_count;i++){
  LinuxRaw*s=&e->raw[i];
  LinuxMerged*m=e->merged_count?&e->merged[e->merged_count-1]:0;
  if(m&&s->page<m->end)return SF_BAD_FORMAT;
  if(m&&s->page==m->end&&plan_flags(s->flags)==m->flags){m->end=s->end;continue;}
  if(e->merged_count==SF_LINUX_SEGMENT_MAX)return SF_NO_SPACE;
  m=&e->merged[e->merged_count++];
  m->start=s->page;m->end=s->end;m->flags=plan_flags(s->flags);
 }
 uint64_t span=e->merged[e->merged_count-1].end;
 int entry_ok=0;
 for(unsigned i=0;i<e->merged_count;i++){const LinuxMerged*m=&e->merged[i];
  if(m->end>SF_IMAGE_MAX)return SF_NO_SPACE;
  if(m->flags!=PLAN_EXEC||entry<m->start||entry>=m->end)continue;
  for(unsigned j=0;j<e->raw_count;j++){const LinuxRaw*s=&e->raw[j];
   if(s->flags==(PF_R|PF_X)&&entry>=s->va&&entry<s->va+s->mem)entry_ok=1;
  }
 }
 if(!entry_ok)return SF_BAD_FORMAT;
 uint64_t phdr=0;
 for(unsigned i=0;i<e->raw_count;i++){const LinuxRaw*s=&e->raw[i];
  uint64_t delta;if(phoff<s->offset||(delta=phoff-s->offset)>s->file)continue;
  if(phnum*ELF_PHENT<=s->file-delta){phdr=s->va+delta;break;}
 }
 if(e->dynamic_bytes){
  uint64_t rela=0,relasz=0;int have_rela=0;
  if(e->dynamic_bytes%16||e->dynamic_bytes/16>256)return SF_BAD_FORMAT;
  for(uint64_t pos=0;pos<e->dynamic_bytes;pos+=16){
   const uint8_t*p=image+e->dynamic_offset+pos;
   uint64_t tag=le64(p),value=le64(p+8);
   if(tag==DT_NULL)break;
   switch(tag){
    case DT_NEEDED:return SF_DEPENDENCY;
    case DT_RELA:rela=value;have_rela=1;break;
    case DT_RELASZ:relasz=value;break;
    case DT_RELAENT:if(value!=24)return SF_BAD_FORMAT;break;
    case DT_REL:case DT_RELSZ:case DT_RELR:case DT_RELRSZ:if(value)return SF_BAD_FORMAT;break;
    case DT_TEXTREL:return SF_BAD_FORMAT;
    case DT_FLAGS:if(value&DF_TEXTREL)return SF_BAD_FORMAT;break;
    default:break;
   }
  }
  if(relasz){
   uint64_t file_offset;
   if(!have_rela||!rela||relasz%24||!vaddr_file(e,rela,relasz,&file_offset))return SF_BAD_FORMAT;
   if(relasz/24>SF_LINUX_RELOCATION_MAX)return SF_BAD_FORMAT;
   e->rela_offset=file_offset;e->rela_bytes=relasz;e->rela_count=(uint32_t)(relasz/24);
  }
  for(uint32_t i=0;i<e->rela_count;i++){
   const uint8_t*p=image+e->rela_offset+(uint64_t)i*24;
   uint64_t target=le64(p),info=le64(p+8);int64_t addend=(int64_t)le64(p+16);
   if(info!=R_X86_64_RELATIVE||(target&7))return SF_BAD_FORMAT;
   int writable=0;
   for(unsigned j=0;j<e->raw_count;j++){const LinuxRaw*s=&e->raw[j];
    if(s->flags!=(PF_R|PF_W)||target<s->va||target-s->va>s->mem)continue;
    if(8<=s->mem-(target-s->va)){writable=1;break;}
   }
   if(!writable)return SF_BAD_FORMAT;
   if(addend<0||(uint64_t)addend>=span)return SF_BAD_FORMAT;
  }
 }
 e->info.entry_rva=entry;e->info.image_bytes=span;e->info.phdr_rva=phdr;
 e->info.phnum=(uint32_t)phnum;e->info.relocations=e->rela_count;
 e->info.syscalls=0;e->info.flags=0;
 return SF_OK;
}
int sf_linux_elf_validate(const uint8_t*image,size_t bytes,SfLinuxInfo*info){
 LinuxElf e;int r=decode(image,bytes,&e);if(r)return r;
 if(info)*info=e.info;
 return SF_OK;
}
int sf_linux_elf_load(const uint8_t*image,size_t bytes,uint64_t base,
                      void*staging,size_t capacity,SfLoadPlan*plan,
                      SfLinuxInfo*info,unsigned flags){
 if(!staging||!plan)return SF_INVALID;
 LinuxElf e;int r=decode(image,bytes,&e);if(r)return r;
 if(!sf_image_base_valid(base)||base<SF_PIE_MIN)return SF_INVALID;
 uint64_t total=0;
 for(unsigned i=0;i<e.merged_count;i++)total+=e.merged[i].end-e.merged[i].start;
 if(total>capacity)return SF_NO_SPACE;
 memset(plan,0,sizeof(*plan));
 plan->size=sizeof(*plan);plan->entry=e.info.entry_rva;
 plan->image_base=base;plan->count=e.merged_count;
 size_t used=0;
 for(unsigned i=0;i<e.merged_count;i++){
  const LinuxMerged*m=&e.merged[i];uint64_t len=m->end-m->start;
  uint8_t*dst=(uint8_t*)staging+used;
  memset(dst,0,(size_t)len);
  for(unsigned j=0;j<e.raw_count;j++){const LinuxRaw*s=&e.raw[j];
   if(s->page<m->start||s->page>=m->end)continue;
   memcpy(dst+(s->va-m->start),image+s->offset,(size_t)s->file);
  }
  plan->segments[i]=(SfLoadSegment){m->start,len,(uint64_t)dst,m->flags,0};
  used+=(size_t)len;
 }
 for(uint32_t i=0;i<e.rela_count;i++){
  const uint8_t*p=image+e.rela_offset+(uint64_t)i*24;
  uint64_t target=le64(p);int64_t addend=(int64_t)le64(p+16);
  int wrote=0;
  for(unsigned j=0;j<plan->count;j++){SfLoadSegment*s=&plan->segments[j];
   if(s->flags!=PLAN_WRITE||target<s->rva||target-s->rva>s->bytes)continue;
   if(8<=s->bytes-(target-s->rva)){put64((uint8_t*)s->data+(target-s->rva),base+(uint64_t)addend);wrote=1;break;}
  }
  if(!wrote)return SF_BAD_FORMAT;
 }
 unsigned rewritten=0;
 if(flags&SF_LINUX_FLAG_REWRITE_SYSCALLS){
  for(unsigned i=0;i<plan->count;i++){SfLoadSegment*s=&plan->segments[i];
   if(s->flags!=PLAN_EXEC)continue;
   uint8_t*p=(uint8_t*)s->data;
   for(uint64_t k=0;k+1<s->bytes;k++)if(p[k]==0x0f&&p[k+1]==0x05){p[k]=0xcd;p[k+1]=0x80;rewritten++;k++;}
  }
 }
 e.info.syscalls=rewritten;
 if(info)*info=e.info;
 return SF_OK;
}
int sf_linux_stack_init(void*buffer,size_t capacity,uint64_t stack_top,
                        uint64_t image_base,const SfLinuxInfo*info,
                        const SfLinuxStackRequest*request,SfLinuxStack*out){
 if(!buffer||!info||!request||!out||!capacity||stack_top<capacity)return SF_INVALID;
 if(!request->argc||request->argc>SF_LINUX_ARG_MAX||request->envc>SF_LINUX_ENV_MAX)return SF_BAD_FORMAT;
 if(!request->argv||(request->envc&&!request->env))return SF_BAD_FORMAT;
 if(!info->phdr_rva||!info->phnum)return SF_BAD_FORMAT;
 uint8_t*buf=(uint8_t*)buffer;uint64_t low=stack_top-capacity;size_t cursor=capacity;
 uint64_t argv_addr[SF_LINUX_ARG_MAX],env_addr[SF_LINUX_ENV_MAX],random_addr=0;
 for(unsigned i=0;i<request->envc;i++){
  size_t n;if(bounded_length(request->env[i],SF_LINUX_STRING_MAX,&n))return SF_BAD_FORMAT;
  if(n+1>cursor)return SF_NO_SPACE;
  cursor-=n+1;memcpy(buf+cursor,request->env[i],n+1);env_addr[i]=low+cursor;
 }
 for(unsigned i=0;i<request->argc;i++){
  size_t n;if(bounded_length(request->argv[i],SF_LINUX_STRING_MAX,&n))return SF_BAD_FORMAT;
  if(n+1>cursor)return SF_NO_SPACE;
  cursor-=n+1;memcpy(buf+cursor,request->argv[i],n+1);argv_addr[i]=low+cursor;
 }
 if(request->random16){
  if(cursor<16)return SF_NO_SPACE;
  cursor-=16;memcpy(buf+cursor,request->random16,16);random_addr=low+cursor;
 }
 cursor&=~(size_t)7;
 size_t argv_bytes=((size_t)request->argc+1)*8;
 size_t envp_bytes=((size_t)request->envc+1)*8;
 size_t aux_bytes=((size_t)(request->random16?12:11)+1)*16;
 size_t need=argv_bytes+envp_bytes+aux_bytes+8;
 if(cursor<need)return SF_NO_SPACE;
 size_t sp_off=cursor-need;
 size_t pad=(16-(unsigned)((low+sp_off)&15))&15;
 if(sp_off<pad)return SF_NO_SPACE;
 sp_off-=pad;
 uint64_t aux[SF_LINUX_AUX_MAX][2];unsigned aux_count=0;
 aux[aux_count][0]=3;aux[aux_count++][1]=image_base+info->phdr_rva;
 aux[aux_count][0]=4;aux[aux_count++][1]=ELF_PHENT;
 aux[aux_count][0]=5;aux[aux_count++][1]=info->phnum;
 aux[aux_count][0]=6;aux[aux_count++][1]=SF_PAGE;
 aux[aux_count][0]=7;aux[aux_count++][1]=0;
 aux[aux_count][0]=9;aux[aux_count++][1]=image_base+info->entry_rva;
 aux[aux_count][0]=11;aux[aux_count++][1]=request->uid;
 aux[aux_count][0]=12;aux[aux_count++][1]=request->uid;
 aux[aux_count][0]=13;aux[aux_count++][1]=request->gid;
 aux[aux_count][0]=14;aux[aux_count++][1]=request->gid;
 aux[aux_count][0]=31;aux[aux_count++][1]=argv_addr[0];
 if(request->random16){aux[aux_count][0]=25;aux[aux_count++][1]=random_addr;}
 aux[aux_count][0]=0;aux[aux_count++][1]=0;
 size_t off=sp_off;
 put64(buf+off,(uint64_t)request->argc);off+=8;
 for(unsigned i=0;i<request->argc;i++,off+=8)put64(buf+off,argv_addr[i]);
 put64(buf+off,0);off+=8;
 for(unsigned i=0;i<request->envc;i++,off+=8)put64(buf+off,env_addr[i]);
 put64(buf+off,0);off+=8;
 for(unsigned i=0;i<aux_count;i++,off+=16){put64(buf+off,aux[i][0]);put64(buf+off+8,aux[i][1]);}
 if(off>capacity)return SF_NO_SPACE;
 out->sp=low+sp_off;out->argc=request->argc;
 out->argv=out->sp+8;out->envp=out->argv+argv_bytes;out->auxv=out->envp+envp_bytes;
 out->bytes=stack_top-out->sp;
 return SF_OK;
}
