#include "linux_image.h"
#define ELF_HEADER 64u
#define ELF_PHENT 56u
#define ELF_SYMENT 24u
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
#define DT_PLTRELSZ 2u
#define DT_HASH 4u
#define DT_STRTAB 5u
#define DT_SYMTAB 6u
#define DT_RELA 7u
#define DT_RELASZ 8u
#define DT_RELAENT 9u
#define DT_STRSZ 10u
#define DT_SYMENT 11u
#define DT_SONAME 14u
#define DT_REL 17u
#define DT_RELSZ 18u
#define DT_RELENT 19u
#define DT_PLTREL 20u
#define DT_TEXTREL 22u
#define DT_JMPREL 23u
#define DT_FLAGS 30u
#define DT_RELRSZ 35u
#define DT_RELR 36u
#define DT_GNU_HASH 0x6ffffef5u
#define DF_TEXTREL 4u
#define DT_PLTREL_RELA 7u
#define R_X86_64_NONE 0u
#define R_X86_64_64 1u
#define R_X86_64_COPY 5u
#define R_X86_64_GLOB_DAT 6u
#define R_X86_64_JUMP_SLOT 7u
#define R_X86_64_RELATIVE 8u
#define R_X86_64_DTPMOD64 16u
#define R_X86_64_DTPOFF64 17u
#define R_X86_64_TPOFF64 18u
#define SHN_UNDEF 0u
#define SHN_ABS 0xfff1u
#define STB_LOCAL 0u
#define STB_GLOBAL 1u
#define STB_WEAK 2u
#define STT_NOTYPE 0u
#define STT_OBJECT 1u
#define STT_FUNC 2u
#define STT_TLS 6u
#define PLAN_READ 1u
#define PLAN_WRITE 3u
#define PLAN_EXEC 5u
typedef struct { uint64_t offset,file,va,mem,page,end,align; uint32_t flags; } LinuxRaw;
typedef struct { uint64_t start,end; uint32_t flags; } LinuxMerged;
typedef struct {
 const uint8_t *image; size_t bytes;
 const char *name; const uint8_t *digest;
 LinuxRaw raw[SF_LINUX_RAW_SEGMENT_MAX]; unsigned raw_count;
 uint64_t span_start,span_end,shift;
 uint64_t entry,phoff,phnum,phdr_va,tls_ph;
 uint64_t dyn_off,dyn_bytes;
 uint64_t strtab,strsz,symtab,syment,symcount;
 const uint8_t *str;
 uint64_t soname; int has_soname;
 uint64_t needed[SF_LINUX_NEEDED_MAX]; unsigned needed_count;
 uint64_t rela_off; uint32_t rela_count;
 uint64_t jmprel_off; uint32_t jmprel_count;
 uint64_t tls_va,tls_filesz,tls_memsz,tls_align;
 uint64_t tls_offset; unsigned tls_index;
 uint32_t flags;
} LinuxObject;
typedef struct {
 LinuxObject obj[SF_LINUX_OBJECT_MAX]; unsigned count;
 uint64_t tls_total,tls_align;
 SfLinuxInfo info;
} LinuxImage;
typedef struct { uint64_t value,tls_offset,size; unsigned module; uint8_t defined,is_tls; } LinuxSym;
static uint16_t le16(const uint8_t*p){return(uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t le32(const uint8_t*p){return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t*p){return(uint64_t)le32(p)|(uint64_t)le32(p+4)<<32;}
static void put64(uint8_t*p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8));}
static uint64_t page_up(uint64_t v){return(v+SF_PAGE-1)&~(SF_PAGE-1);}
static uint64_t align_up(uint64_t v,uint64_t a){return a?(v+a-1)&~(a-1):v;}
static uint32_t plan_flags(uint32_t elf){
 if(elf==(PF_R|PF_X))return PLAN_EXEC;
 if(elf==(PF_R|PF_W))return PLAN_WRITE;
 return PLAN_READ;
}
static int vaddr_file(const LinuxObject*o,uint64_t va,uint64_t len,uint64_t*out){
 for(unsigned i=0;i<o->raw_count;i++){const LinuxRaw*s=&o->raw[i];
  if(va<s->va||va-s->va>=s->file||len>s->file-(va-s->va))continue;
  *out=s->offset+(va-s->va);return 1;
 }
 return 0;
}
static int bounded_length(const char*text,size_t limit,size_t*out){
 if(!text||!out)return 0;
 for(size_t i=0;i<limit;i++)if(!text[i]){*out=i;return 1;}
 return 0;
}
static int object_string(const LinuxObject*o,uint64_t off,const char**out,size_t*len){
 if(!o->str||off>=o->strsz)return 0;
 const char*s=(const char*)o->str+off;
 size_t n;if(!bounded_length(s,o->strsz-off,&n))return 0;
 if(out)*out=s;
 if(len)*len=n;
 return 1;
}
static int name_matches(const char*a,const char*b){
 if(!a||!b)return 0;
 while(*a&&*b){if(*a++!=*b++)return 0;}
 return *a==*b;
}
/* ---- SHA-256（仅用于库摘要核验） ---- */
static uint32_t ror32(uint32_t v,unsigned n){return(v>>n)|(v<<(32-n));}
static void sha256_block(uint32_t h[8],const uint8_t*p){
 static const uint32_t k[64]={
  0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
  0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
  0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
  0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
  0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
  0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
  0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
  0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
 uint32_t w[64];
 for(unsigned i=0;i<16;i++)w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
 for(unsigned i=16;i<64;i++){uint32_t s0=ror32(w[i-15],7)^ror32(w[i-15],18)^(w[i-15]>>3);
  uint32_t s1=ror32(w[i-2],17)^ror32(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
 uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],i=h[7];
 for(unsigned j=0;j<64;j++){uint32_t s1=ror32(e,6)^ror32(e,11)^ror32(e,25);
  uint32_t ch=(e&f)^(~e&g);uint32_t t1=i+s1+ch+k[j]+w[j];
  uint32_t s0=ror32(a,2)^ror32(a,13)^ror32(a,22);uint32_t maj=(a&b)^(a&c)^(b&c);
  uint32_t t2=s0+maj;i=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
 h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=i;
}
static void sha256(const uint8_t*data,size_t bytes,uint8_t out[32]){
 uint32_t h[8]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
 size_t full=bytes&~(size_t)63;
 for(size_t i=0;i<full;i+=64)sha256_block(h,data+i);
 uint8_t tail[128];size_t rem=bytes-full;memset(tail,0,sizeof(tail));memcpy(tail,data+full,rem);
 tail[rem]=0x80;size_t total=rem+1<=56?64:128;
 uint64_t bits=(uint64_t)bytes*8;for(unsigned i=0;i<8;i++)tail[total-1-i]=(uint8_t)(bits>>(i*8));
 sha256_block(h,tail);if(total==128)sha256_block(h,tail+64);
 for(unsigned i=0;i<8;i++){out[i*4]=(uint8_t)(h[i]>>24);out[i*4+1]=(uint8_t)(h[i]>>16);out[i*4+2]=(uint8_t)(h[i]>>8);out[i*4+3]=(uint8_t)h[i];}
}
/* ---- ELF 解析 ---- */
static int parse_dynamic(LinuxObject*o){
 if(!o->dyn_bytes)return SF_OK;
 if(o->dyn_bytes%16||o->dyn_bytes/16>512)return SF_BAD_FORMAT;
 uint64_t rela_va=0,relasz=0,jmprel_va=0,pltrelsz=0,pltrel=0;
 int have_sym=0;
 for(uint64_t pos=0;pos<o->dyn_bytes;pos+=16){
  const uint8_t*p=o->image+o->dyn_off+pos;
  uint64_t tag=le64(p),value=le64(p+8);
  if(tag==DT_NULL)break;
  switch(tag){
   case DT_NEEDED:
    if(o->needed_count==SF_LINUX_NEEDED_MAX)return SF_NO_SPACE;
    o->needed[o->needed_count++]=value;break;
   case DT_SONAME:o->has_soname=1;o->soname=value;break;
   case DT_STRTAB:o->strtab=value;break;
   case DT_STRSZ:o->strsz=value;break;
   case DT_SYMTAB:o->symtab=value;have_sym=1;break;
   case DT_SYMENT:o->syment=value;break;
   case DT_HASH:o->symcount=0;{uint64_t f;uint32_t nchain=0;if(vaddr_file(o,value,8,&f))nchain=le32(o->image+f+4);o->symcount=nchain;}break;
   case DT_GNU_HASH:break;
   case DT_RELA:rela_va=value;break;
   case DT_RELASZ:relasz=value;break;
   case DT_RELAENT:if(value!=ELF_SYMENT)return SF_BAD_FORMAT;break;
   case DT_REL:case DT_RELSZ:case DT_RELENT:if(value)return SF_BAD_FORMAT;break;
   case DT_RELR:case DT_RELRSZ:if(value)return SF_BAD_FORMAT;break;
   case DT_JMPREL:jmprel_va=value;break;
   case DT_PLTRELSZ:pltrelsz=value;break;
   case DT_PLTREL:pltrel=value;break;
   case DT_TEXTREL:return SF_BAD_FORMAT;
   case DT_FLAGS:if(value&DF_TEXTREL)return SF_BAD_FORMAT;break;
   default:break;
  }
 }
 if(o->strsz){uint64_t f;if(!vaddr_file(o,o->strtab,o->strsz,&f))return SF_BAD_FORMAT;o->str=o->image+f;}
 else if(o->needed_count||o->has_soname)return SF_BAD_FORMAT;
 if(o->has_soname&&!object_string(o,o->soname,0,0))return SF_BAD_FORMAT;
 for(unsigned i=0;i<o->needed_count;i++)if(!object_string(o,o->needed[i],0,0))return SF_BAD_FORMAT;
 if(have_sym&&o->syment!=ELF_SYMENT)return SF_BAD_FORMAT;
 if(!o->symcount){
  const uint8_t*gnu=0;uint64_t gnu_f=0;
  /* 无 DT_HASH 时用 GNU_HASH 推导符号数。 */
  for(uint64_t pos=0;pos<o->dyn_bytes;pos+=16){const uint8_t*p=o->image+o->dyn_off+pos;
   if(le64(p)==DT_NULL)break;
   if(le64(p)==DT_GNU_HASH){uint64_t v=le64(p+8);if(!vaddr_file(o,v,16,&gnu_f))return SF_BAD_FORMAT;gnu=o->image+gnu_f;break;}}
  if(gnu){
   uint32_t nbucket=le32(gnu),symoff=le32(gnu+4),bloom=le32(gnu+8);
   uint64_t buckets=gnu_f+16+(uint64_t)bloom*8;
   uint64_t chains=buckets+(uint64_t)nbucket*4;
   uint64_t chains_file;if(!vaddr_file(o,buckets,(uint64_t)nbucket*4+4,&chains_file))return SF_BAD_FORMAT;
   if(!vaddr_file(o,chains,4,&chains_file))return SF_BAD_FORMAT;
   const LinuxRaw*raw=0;for(unsigned i=0;i<o->raw_count;i++)if(o->raw[i].offset<=chains_file&&chains_file<o->raw[i].offset+o->raw[i].file){raw=&o->raw[i];break;}
   if(!raw)return SF_BAD_FORMAT;
   uint64_t chain_limit=(raw->offset+raw->file-chains_file)/4;
   uint64_t maxsym=symoff;
   for(uint32_t i=0;i<nbucket;i++){
    const uint8_t*bk=o->image+chains_file-((uint64_t)nbucket*4)+((uint64_t)i*4);
    uint32_t sym=le32(bk);if(!sym)continue;
    for(;;){
     if(sym<symoff||sym>=SF_LINUX_SYMBOL_MAX)return SF_BAD_FORMAT;
     uint64_t idx=sym-symoff;
     if(idx>=chain_limit)return SF_BAD_FORMAT;
     uint32_t chain=le32(o->image+chains_file+idx*4);
     if(chain&1){sym++;break;}
     sym++;
    }
    if(sym>maxsym)maxsym=sym;
   }
   o->symcount=maxsym;
  }
 }
 if(o->symcount>SF_LINUX_SYMBOL_MAX)return SF_BAD_FORMAT;
 if(o->symcount){uint64_t f;if(!vaddr_file(o,o->symtab,o->symcount*ELF_SYMENT,&f))return SF_BAD_FORMAT;}
 if(relasz){
  uint64_t f;
  if(relasz%ELF_SYMENT||relasz/ELF_SYMENT>SF_LINUX_RELOCATION_MAX)return SF_BAD_FORMAT;
  if(!rela_va||!vaddr_file(o,rela_va,relasz,&f))return SF_BAD_FORMAT;
  o->rela_off=f;o->rela_count=(uint32_t)(relasz/ELF_SYMENT);
 }
 if(pltrelsz){
  uint64_t f;
  if(pltrel!=DT_PLTREL_RELA||!jmprel_va||pltrelsz%ELF_SYMENT)return SF_BAD_FORMAT;
  if(pltrelsz/ELF_SYMENT>SF_LINUX_RELOCATION_MAX)return SF_BAD_FORMAT;
  if(!vaddr_file(o,jmprel_va,pltrelsz,&f))return SF_BAD_FORMAT;
  o->jmprel_off=f;o->jmprel_count=(uint32_t)(pltrelsz/ELF_SYMENT);
 }
 o->flags|=SF_LINUX_INFO_DYNAMIC;
 return SF_OK;
}
static int check_relocations(const LinuxObject*o){
 for(int range=0;range<2;range++){
  uint64_t off=range?o->jmprel_off:o->rela_off;uint32_t n=range?o->jmprel_count:o->rela_count;
  for(uint32_t i=0;i<n;i++){
   const uint8_t*p=o->image+off+(uint64_t)i*ELF_SYMENT;
   uint64_t r_offset=le64(p),r_info=le64(p+8);int64_t addend=(int64_t)le64(p+16);
   uint32_t type=(uint32_t)(r_info&0xffffffffu),sym=(uint32_t)(r_info>>32);
   if(type==R_X86_64_NONE)continue;
   if(type!=R_X86_64_64&&type!=R_X86_64_COPY&&type!=R_X86_64_GLOB_DAT&&type!=R_X86_64_JUMP_SLOT&&
      type!=R_X86_64_RELATIVE&&type!=R_X86_64_DTPMOD64&&type!=R_X86_64_DTPOFF64&&type!=R_X86_64_TPOFF64)
    return SF_BAD_FORMAT;
   if(sym&&sym>=o->symcount)return SF_BAD_FORMAT;
   if((r_offset&7))return SF_BAD_FORMAT;
   int writable=0;
   for(unsigned j=0;j<o->raw_count;j++){const LinuxRaw*s=&o->raw[j];
    if(s->flags!=(PF_R|PF_W)||r_offset<s->va||r_offset-s->va>=s->mem)continue;
    if(s->mem-(r_offset-s->va)<8)return SF_BAD_FORMAT;
    writable=1;break;
   }
   if(!writable)return SF_BAD_FORMAT;
   if(type==R_X86_64_RELATIVE&&(addend<0||(uint64_t)addend>=o->span_end))return SF_BAD_FORMAT;
  }
 }
 return SF_OK;
}
static int parse_object(LinuxObject*o,const uint8_t*image,size_t bytes,
                        const char*name,const uint8_t*digest,int require_entry){
 if(!image||!o||bytes<ELF_HEADER)return SF_BAD_FORMAT;
 memset(o,0,sizeof(*o));o->image=image;o->bytes=bytes;o->name=name;o->digest=digest;
 if(memcmp(image,"\x7f""ELF",4)||image[4]!=2||image[5]!=1||image[6]!=1)return SF_BAD_FORMAT;
 if(le16(image+16)!=ET_DYN||le16(image+18)!=EM_X86_64||le32(image+20)!=1)return SF_BAD_FORMAT;
 if(le16(image+52)!=ELF_HEADER||le16(image+54)!=ELF_PHENT||le16(image+58)!=ELF_HEADER)return SF_BAD_FORMAT;
 uint64_t phoff=le64(image+32),phnum=le16(image+56);o->entry=le64(image+24);
 if(!phnum||phnum>SF_LINUX_RAW_SEGMENT_MAX)return SF_BAD_FORMAT;
 if(phoff>bytes||phnum*ELF_PHENT>bytes-phoff)return SF_BAD_FORMAT;
 if(o->entry>=SF_IMAGE_MAX)return SF_BAD_FORMAT;
 o->phoff=phoff;o->phnum=phnum;
 for(uint64_t i=0;i<phnum;i++){
  const uint8_t*p=image+phoff+i*ELF_PHENT;
  uint32_t type=le32(p),flags=le32(p+4);
  uint64_t offset=le64(p+8),va=le64(p+16),file=le64(p+32),mem=le64(p+40),align=le64(p+48);
  if(type==PT_INTERP){
   if(!file||offset>bytes||file>bytes-offset)return SF_BAD_FORMAT;
   size_t n;if(!bounded_length((const char*)image+offset,file<SF_LINUX_STRING_MAX?file:SF_LINUX_STRING_MAX,&n))return SF_BAD_FORMAT;
   o->flags|=SF_LINUX_INFO_INTERP;continue;
  }
  if(type==PT_TLS){
   if(!mem||file>mem||va>=SF_IMAGE_MAX||mem>SF_IMAGE_MAX-va)return SF_BAD_FORMAT;
   if(align&&((align&(align-1))||align>0x200000))return SF_BAD_FORMAT;
   o->tls_va=va;o->tls_filesz=file;o->tls_memsz=mem;o->tls_align=align?align:1;o->tls_ph=i+1;
   o->flags|=SF_LINUX_INFO_TLS;continue;
  }
  if(type==PT_DYNAMIC){
   if(!mem||offset>bytes||mem>bytes-offset)return SF_BAD_FORMAT;
   o->dyn_off=offset;o->dyn_bytes=mem;continue;
  }
  if(type!=PT_LOAD)continue;
  if(!mem||file>mem)return SF_BAD_FORMAT;
  if(flags!=PF_R&&flags!=(PF_R|PF_W)&&flags!=(PF_R|PF_X))return SF_BAD_FORMAT;
  if(mem>SF_IMAGE_MAX||va>=SF_IMAGE_MAX||mem>SF_IMAGE_MAX-va)return SF_NO_SPACE;
  if(align&&((align&(align-1))||align>0x200000||(va%align)!=(offset%align)))return SF_BAD_FORMAT;
  uint64_t delta=va&(SF_PAGE-1);
  if((offset&(SF_PAGE-1))!=delta)return SF_BAD_FORMAT;
  if(offset>bytes||file>bytes-offset)return SF_BAD_FORMAT;
  if(o->raw_count==SF_LINUX_RAW_SEGMENT_MAX)return SF_NO_SPACE;
  LinuxRaw*s=&o->raw[o->raw_count++];
  s->offset=offset;s->file=file;s->va=va;s->mem=mem;s->flags=flags;s->align=align;
  s->page=va-delta;s->end=page_up(va+mem);
 }
 if(!o->raw_count)return SF_BAD_FORMAT;
 if(o->tls_memsz&&o->tls_filesz){uint64_t f;if(!vaddr_file(o,o->tls_va,o->tls_filesz,&f))return SF_BAD_FORMAT;}
 for(unsigned i=1;i<o->raw_count;i++){
  LinuxRaw t=o->raw[i];unsigned j=i;
  while(j&&o->raw[j-1].page>t.page){o->raw[j]=o->raw[j-1];j--;}
  o->raw[j]=t;
 }
 for(unsigned i=0;i<o->raw_count;i++){
  const LinuxRaw*s=&o->raw[i];
  if(i&&s->page<o->raw[i-1].end)return SF_BAD_FORMAT;
 }
 o->span_start=o->raw[0].page;o->span_end=o->raw[0].end;
 for(unsigned i=1;i<o->raw_count;i++)if(o->raw[i].end>o->span_end)o->span_end=o->raw[i].end;
 int entry_ok=0;
 for(unsigned i=0;i<o->raw_count;i++){const LinuxRaw*s=&o->raw[i];
  if(s->flags==(PF_R|PF_X)&&o->entry>=s->va&&o->entry<s->va+s->mem)entry_ok=1;
 }
 if(require_entry&&!entry_ok)return SF_BAD_FORMAT;
 for(unsigned i=0;i<o->raw_count;i++){const LinuxRaw*s=&o->raw[i];
  uint64_t delta;if(phoff<s->offset||(delta=phoff-s->offset)>s->file)continue;
  if(phnum*ELF_PHENT<=s->file-delta){o->phdr_va=s->va+delta;break;}
 }
 int r=parse_dynamic(o);if(r)return r;
 r=check_relocations(o);if(r)return r;
 if(digest){uint8_t sum[32];sha256(image,bytes,sum);if(memcmp(sum,digest,32))return SF_BAD_FORMAT;}
 return SF_OK;
}
/* ---- 布局与装载 ---- */
typedef struct { const LinuxObject*o; LinuxMerged m[SF_LINUX_SEGMENT_MAX]; unsigned count; } LinuxObjPlan;
static int layout(LinuxImage*img,uint64_t base,uint64_t*out_span){
 uint64_t cursor=0;
 for(unsigned k=0;k<img->count;k++){
  LinuxObject*o=&img->obj[k];
  uint64_t align=SF_PAGE;
  for(unsigned i=0;i<o->raw_count;i++)if(o->raw[i].align>align)align=o->raw[i].align;
  if(align>0x200000)return SF_BAD_FORMAT;
  uint64_t start=align_up(cursor,align);
  o->shift=start-o->span_start;
  cursor=start+(o->span_end-o->span_start);
  if(cursor>SF_IMAGE_MAX)return SF_NO_SPACE;
 }
 uint64_t tls_start=0;
 if(img->tls_total){
  LinuxObject*main=&img->obj[0];
  if(!main->tls_memsz)return SF_BAD_FORMAT;
  tls_start=align_up(cursor,img->tls_align);
  cursor=tls_start+img->tls_total;
  if(cursor>SF_IMAGE_MAX)return SF_NO_SPACE;
 }
 *out_span=cursor;
 (void)base;return SF_OK;
}
static int build_plan(LinuxImage*img,uint64_t base,uint64_t tls_start,uint64_t span,
                      void*staging,size_t capacity,SfLoadPlan*plan){
 LinuxMerged merged[SF_LINUX_SEGMENT_MAX];unsigned merged_count=0;
 for(unsigned k=0;k<img->count;k++){
  const LinuxObject*o=&img->obj[k];
  LinuxMerged local[SF_LINUX_SEGMENT_MAX];unsigned n=0;
  for(unsigned i=0;i<o->raw_count;i++){
   const LinuxRaw*s=&o->raw[i];uint64_t st=s->page+o->shift,en=s->end+o->shift;uint32_t fl=plan_flags(s->flags);
   if(n&&local[n-1].end==st&&local[n-1].flags==fl){local[n-1].end=en;continue;}
   if(n==SF_LINUX_SEGMENT_MAX)return SF_NO_SPACE;
   local[n].start=st;local[n].end=en;local[n].flags=fl;n++;
  }
  for(unsigned i=0;i<n;i++){
   LinuxMerged*m=&local[i];
   if(merged_count&&merged[merged_count-1].end==m->start&&merged[merged_count-1].flags==m->flags){merged[merged_count-1].end=m->end;continue;}
   if(merged_count==SF_LINUX_SEGMENT_MAX)return SF_NO_SPACE;
   merged[merged_count++]=*m;
  }
 }
 if(img->tls_total){
  if(merged_count==SF_LINUX_SEGMENT_MAX)return SF_NO_SPACE;
  merged[merged_count].start=tls_start;merged[merged_count].end=tls_start+img->tls_total;
  merged[merged_count].flags=PLAN_WRITE;merged_count++;
 }
 if(!merged_count)return SF_BAD_FORMAT;
 uint64_t total=0;
 for(unsigned i=0;i<merged_count;i++){if(merged[i].end>SF_IMAGE_MAX)return SF_NO_SPACE;total+=merged[i].end-merged[i].start;}
 if(total>capacity)return SF_NO_SPACE;
 memset(plan,0,sizeof(*plan));
 plan->size=sizeof(*plan);plan->image_base=base;plan->count=merged_count;
 size_t used=0;
 for(unsigned i=0;i<merged_count;i++){
  const LinuxMerged*m=&merged[i];uint64_t len=m->end-m->start;
  uint8_t*dst=(uint8_t*)staging+used;
  memset(dst,0,(size_t)len);
  plan->segments[i]=(SfLoadSegment){m->start,len,(uint64_t)dst,m->flags,0};
  used+=(size_t)len;
 }
 for(unsigned k=0;k<img->count;k++){
  const LinuxObject*o=&img->obj[k];
  for(unsigned j=0;j<o->raw_count;j++){
   const LinuxRaw*s=&o->raw[j];uint64_t va=s->va+o->shift;
   for(unsigned i=0;i<plan->count;i++){SfLoadSegment*g=&plan->segments[i];
    if(va<g->rva||va>=g->rva+g->bytes)continue;
    memcpy((uint8_t*)g->data+(va-g->rva),o->image+s->offset,(size_t)s->file);break;
   }
  }
 }
 if(img->tls_total){
  for(unsigned k=0;k<img->count;k++){const LinuxObject*o=&img->obj[k];
   if(!o->tls_filesz)continue;
   uint64_t f;
   if(!vaddr_file(o,o->tls_va,o->tls_filesz,&f))return SF_BAD_FORMAT;
   for(unsigned i=0;i<plan->count;i++){SfLoadSegment*g=&plan->segments[i];
    if(tls_start<g->rva||tls_start>=g->rva+g->bytes)continue;
    memcpy((uint8_t*)g->data+(tls_start-g->rva)+o->tls_offset,o->image+f,(size_t)o->tls_filesz);break;
   }
  }
 }
 plan->entry=img->obj[0].entry+img->obj[0].shift;
 (void)span;
 return SF_OK;
}
/* ---- 符号与重定位 ---- */
static const uint8_t*object_symbol(const LinuxObject*o,uint32_t index){
 if(index>=o->symcount)return 0;
 uint64_t f;if(!vaddr_file(o,o->symtab+index*ELF_SYMENT,ELF_SYMENT,&f))return 0;
 return o->image+f;
}
static int symbol_name_eq(const LinuxObject*o,uint32_t off,const char*name){
 if(!o->str||off>=o->strsz||!name)return 0;
 const char*s=(const char*)o->str+off;size_t max=o->strsz-off;size_t i=0;
 for(;i<max&&s[i];i++)if(s[i]!=name[i])return 0;
 if(i>=max)return 0;
 return name[i]==0;
}
static int sym_lookup(const LinuxImage*img,uint64_t base,const char*name,LinuxSym*out){
 int weak=0;
 for(unsigned k=0;k<img->count;k++){
  const LinuxObject*o=&img->obj[k];
  for(uint32_t i=1;i<o->symcount;i++){
   const uint8_t*sym=object_symbol(o,i);if(!sym)continue;
   uint32_t nameoff=le32(sym);unsigned bind=(sym[4]>>4)&15;unsigned type=sym[4]&15;
   uint16_t shndx=le16(sym+6);
   if(bind==STB_LOCAL)continue;
   if(type!=STT_NOTYPE&&type!=STT_OBJECT&&type!=STT_FUNC&&type!=STT_TLS)continue;
   if(!symbol_name_eq(o,nameoff,name))continue;
   if(shndx==SHN_UNDEF){if(bind==STB_WEAK)weak=1;continue;}
   memset(out,0,sizeof(*out));out->defined=1;out->size=le64(sym+16);
   uint64_t value=le64(sym+8);
   if(type==STT_TLS){out->is_tls=1;out->module=o->tls_index;out->tls_offset=o->tls_offset+value;}
   else out->value=shndx==SHN_ABS?value:base+o->shift+value;
   return SF_OK;
  }
 }
 if(out)memset(out,0,sizeof(*out));
 return weak?SF_OK:SF_DEPENDENCY;
}
static int reloc_write(LinuxImage*img,SfLoadPlan*plan,uint64_t base,uint64_t tls_total,
                       const LinuxObject*o,uint32_t sym_index,uint32_t type,
                       int64_t addend,uint64_t target){
 int have_sym=0;LinuxSym s;memset(&s,0,sizeof(s));
 if(sym_index){
  const uint8_t*sym=object_symbol(o,sym_index);if(!sym)return SF_BAD_FORMAT;
  uint32_t nameoff=le32(sym);
  if(nameoff>=o->strsz)return SF_BAD_FORMAT;
  const char*sname=(const char*)o->str+nameoff;
  int r=sym_lookup(img,base,sname,&s);
  if(r)return r;
  have_sym=1;
 }
 for(unsigned i=0;i<plan->count;i++){
  SfLoadSegment*g=&plan->segments[i];
  if(g->flags!=PLAN_WRITE||target<g->rva||target>=g->rva+g->bytes)continue;
  uint64_t off=target-g->rva;
  switch(type){
   case R_X86_64_RELATIVE:
    if(off+8>g->bytes||addend<0||(uint64_t)addend>=o->span_end)return SF_BAD_FORMAT;
    put64((uint8_t*)g->data+off,base+o->shift+(uint64_t)addend);return SF_OK;
   case R_X86_64_GLOB_DAT:case R_X86_64_JUMP_SLOT:case R_X86_64_64:{
    if(off+8>g->bytes)return SF_BAD_FORMAT;
    uint64_t v=0;
    if(have_sym&&s.defined)v=s.value;
    else if(have_sym&&!s.defined)v=0;
    if(type==R_X86_64_64)v+=(uint64_t)addend;
    put64((uint8_t*)g->data+off,v);return SF_OK;}
   case R_X86_64_TPOFF64:
    if(off+8>g->bytes||!have_sym||!s.defined||!s.is_tls)return SF_BAD_FORMAT;
    put64((uint8_t*)g->data+off,s.tls_offset-tls_total);return SF_OK;
   case R_X86_64_DTPMOD64:
    if(off+8>g->bytes||!have_sym||!s.defined||!s.is_tls)return SF_BAD_FORMAT;
    put64((uint8_t*)g->data+off,s.module);return SF_OK;
   case R_X86_64_DTPOFF64:
    if(off+8>g->bytes||!have_sym||!s.defined||!s.is_tls)return SF_BAD_FORMAT;
    put64((uint8_t*)g->data+off,s.tls_offset);return SF_OK;
   case R_X86_64_COPY:{
    if(!have_sym||!s.defined||s.is_tls||!s.size)return SF_BAD_FORMAT;
    if(off+s.size>g->bytes||s.value<base)return SF_BAD_FORMAT;
    uint64_t src=s.value-base;
    for(unsigned j=0;j<plan->count;j++){SfLoadSegment*r=&plan->segments[j];
     if(src<r->rva||src>=r->rva+r->bytes)continue;
     if(s.size>r->bytes-(src-r->rva))return SF_BAD_FORMAT;
     memcpy((uint8_t*)g->data+off,(uint8_t*)r->data+(src-r->rva),(size_t)s.size);return SF_OK;}
    return SF_BAD_FORMAT;}
   default:return SF_BAD_FORMAT;
  }
 }
 return SF_BAD_FORMAT;
}
static int apply_relocations(LinuxImage*img,SfLoadPlan*plan,uint64_t base,uint64_t tls_total,uint64_t*lx_count){
 uint64_t count=0;
 for(unsigned k=0;k<img->count;k++){
  LinuxObject*o=&img->obj[k];
  for(int range=0;range<2;range++){
   uint64_t off=range?o->jmprel_off:o->rela_off;uint32_t n=range?o->jmprel_count:o->rela_count;
   for(uint32_t i=0;i<n;i++){
    const uint8_t*p=o->image+off+(uint64_t)i*ELF_SYMENT;
    uint64_t r_offset=le64(p),r_info=le64(p+8);int64_t addend=(int64_t)le64(p+16);
    uint32_t type=(uint32_t)(r_info&0xffffffffu),sym=(uint32_t)(r_info>>32);
    if(type==R_X86_64_NONE)continue;
    if((r_offset&7)||r_offset>=o->span_end)return SF_BAD_FORMAT;
    uint64_t target=o->shift+r_offset;
    int r=reloc_write(img,plan,base,tls_total,o,sym,type,addend,target);
    if(r)return r;
    count++;
    if(count>SF_LINUX_RELOCATION_MAX)return SF_BAD_FORMAT;
   }
  }
 }
 *lx_count=count;return SF_OK;
}
static int patch_tls_phdr(LinuxImage*img,SfLoadPlan*plan,uint64_t tls_start){
 LinuxObject*main=&img->obj[0];
 if(!img->tls_total)return SF_OK;
 if(!main->tls_ph)return SF_BAD_FORMAT;
 uint64_t phoff=main->phoff+ (main->tls_ph-1)*ELF_PHENT;
 for(unsigned i=0;i<main->raw_count;i++){
  const LinuxRaw*s=&main->raw[i];uint64_t delta;
  if(phoff<s->offset||(delta=phoff-s->offset)>=s->file)continue;
  uint64_t va=s->va+main->shift+delta;
  for(unsigned j=0;j<plan->count;j++){SfLoadSegment*g=&plan->segments[j];
   if(va<g->rva||va>=g->rva+g->bytes)continue;
   uint8_t*p=(uint8_t*)g->data+(va-g->rva);
   put64(p+8,0);put64(p+16,tls_start);put64(p+32,img->tls_total);put64(p+40,img->tls_total);put64(p+48,img->tls_align);
   return SF_OK;
  }
 }
 return SF_BAD_FORMAT;
}
static int object_soname(const LinuxObject*o,const char**name){
 if(o->has_soname)return object_string(o,o->soname,name,0);
 if(o->name){*name=o->name;return 1;}
 return 0;
}
static int resolve_deps(LinuxImage*img){
 for(unsigned i=0;i<SF_LINUX_OBJECT_MAX;i++){img->obj[i].tls_index=0;img->obj[i].tls_offset=0;}
 for(unsigned i=0;i<img->count;i++){
  LinuxObject*o=&img->obj[i];
  for(unsigned n=0;n<o->needed_count;n++){
   const char*name;if(!object_string(o,o->needed[n],&name,0))return SF_BAD_FORMAT;
   int found=0;
   for(unsigned j=0;j<img->count;j++){
    const char*cand;if(!object_soname(&img->obj[j],&cand))continue;
    if(name_matches(cand,name)){found=1;break;}
   }
   if(!found)return SF_DEPENDENCY;
  }
 }
 for(unsigned i=1;i<img->count;i++){
  const char*name;if(!object_soname(&img->obj[i],&name))return SF_BAD_FORMAT;
  for(unsigned j=1;j<i;j++){const char*other;if(object_soname(&img->obj[j],&other)&&name_matches(name,other))return SF_BAD_FORMAT;}
 }
 return SF_OK;
}
static void layout_tls(LinuxImage*img){
 uint64_t total=0,align=1;
 for(unsigned i=0;i<img->count;i++){
  LinuxObject*o=&img->obj[i];
  if(!o->tls_memsz)continue;
  if(o->tls_align>align)align=o->tls_align;
  total=align_up(total,o->tls_align);
  o->tls_offset=total;total+=o->tls_memsz;
 }
 if(total)total=align_up(total,align);
 img->tls_total=total;img->tls_align=align;
 unsigned module=0;
 for(unsigned i=0;i<img->count;i++)if(img->obj[i].tls_memsz)img->obj[i].tls_index=++module;
}
int sf_linux_image_load(const SfLinuxObject*objects,unsigned count,uint64_t base,
                        void*staging,size_t capacity,SfLoadPlan*plan,
                        SfLinuxInfo*info,unsigned flags){
 if(!objects||!count||count>SF_LINUX_OBJECT_MAX||!staging||!plan||!capacity)return SF_INVALID;
 if(!sf_image_base_valid(base)||base<SF_PIE_MIN)return SF_INVALID;
 static LinuxImage img;
 memset(&img,0,sizeof(img));img.count=count;
 for(unsigned i=0;i<count;i++){
  int r=parse_object(&img.obj[i],objects[i].image,objects[i].bytes,objects[i].name,objects[i].digest32,i==0);
  if(r)return r;
 }
 int r=resolve_deps(&img);if(r)return r;
 layout_tls(&img);
 uint64_t span=0;r=layout(&img,base,&span);if(r)return r;
 uint64_t tls_start=img.tls_total?align_up(span-img.tls_total,img.tls_align):0;
 /* layout() 返回的 span 已包含 TLS，TLS 起点可由 span 反推。 */
 r=build_plan(&img,base,tls_start,span,staging,capacity,plan);if(r)return r;
 uint64_t relocations=0;
 r=apply_relocations(&img,plan,base,img.tls_total,&relocations);if(r)return r;
 r=patch_tls_phdr(&img,plan,tls_start);if(r)return r;
 unsigned rewritten=0;
 if(flags&SF_LINUX_FLAG_REWRITE_SYSCALLS){
  for(unsigned i=0;i<plan->count;i++){SfLoadSegment*s=&plan->segments[i];
   if(s->flags!=PLAN_EXEC)continue;
   uint8_t*p=(uint8_t*)s->data;
   for(uint64_t k=0;k+1<s->bytes;k++)if(p[k]==0x0f&&p[k+1]==0x05){p[k]=0xcd;p[k+1]=0x80;rewritten++;k++;}
  }
 }
 img.info.entry_rva=plan->entry;
 img.info.image_bytes=span;
 img.info.phdr_rva=img.obj[0].phdr_va+img.obj[0].shift;
 img.info.phnum=(uint32_t)img.obj[0].phnum;
 img.info.relocations=(uint32_t)relocations;
 img.info.syscalls=rewritten;
 img.info.flags=0;
 for(unsigned i=0;i<count;i++)img.info.flags|=img.obj[i].flags;
 img.info.object_count=count;
 img.info.tls_size=(uint32_t)img.tls_total;
 img.info.tls_align=(uint32_t)img.tls_align;
 if(info)*info=img.info;
 return SF_OK;
}
int sf_linux_elf_validate(const uint8_t*image,size_t bytes,SfLinuxInfo*info){
 static LinuxObject o;
 int r=parse_object(&o,image,bytes,0,0,1);if(r)return r;
 if(o.needed_count)return SF_DEPENDENCY;
 if(info){
  memset(info,0,sizeof(*info));
  info->entry_rva=o.entry;info->image_bytes=o.span_end;info->phdr_rva=o.phdr_va;
  info->phnum=(uint32_t)o.phnum;info->relocations=o.rela_count+o.jmprel_count;
  info->syscalls=0;info->flags=o.flags;info->object_count=1;
  info->tls_size=(uint32_t)o.tls_memsz;info->tls_align=(uint32_t)o.tls_align;
 }
 return SF_OK;
}
int sf_linux_elf_load(const uint8_t*image,size_t bytes,uint64_t base,
                      void*staging,size_t capacity,SfLoadPlan*plan,
                      SfLinuxInfo*info,unsigned flags){
 SfLinuxObject object={0,image,bytes,0};
 return sf_linux_image_load(&object,1,base,staging,capacity,plan,info,flags);
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
  size_t n;if(!bounded_length(request->env[i],SF_LINUX_STRING_MAX,&n))return SF_BAD_FORMAT;
  if(n+1>cursor)return SF_NO_SPACE;
  cursor-=n+1;memcpy(buf+cursor,request->env[i],n+1);env_addr[i]=low+cursor;
 }
 for(unsigned i=0;i<request->argc;i++){
  size_t n;if(!bounded_length(request->argv[i],SF_LINUX_STRING_MAX,&n))return SF_BAD_FORMAT;
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
