#include "package.h"
#include "monocypher-ed25519.h"
uint16_t sf_u16(const uint8_t*p){return p[0]|(uint16_t)p[1]<<8;}
uint32_t sf_u32(const uint8_t*p){return sf_u16(p)|(uint32_t)sf_u16(p+2)<<16;}
uint64_t sf_u64(const uint8_t*p){return sf_u32(p)|(uint64_t)sf_u32(p+4)<<32;}
static int span(uint64_t a,uint64_t n,uint64_t limit){return a<=limit&&n<=limit-a;}
static int zero(const uint8_t*p,uint64_t n){while(n--)if(*p++)return 0;return 1;}
static int utf8(const uint8_t*p,uint64_t n){
 while(n){uint32_t c=*p++;n--;if(!c)return 0;if(c<128)continue;int k;uint32_t min;
 if(c>=0xc2&&c<=0xdf){k=1;c&=31;min=128;}else if(c>=0xe0&&c<=0xef){k=2;c&=15;min=2048;}else if(c>=0xf0&&c<=0xf4){k=3;c&=7;min=65536;}else return 0;
 if(n<(uint64_t)k)return 0;while(k--){if((*p&192)!=128)return 0;c=(c<<6)|(*p++&63);n--;}
 if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return 0;
 }return 1;
}
typedef struct {const uint8_t*p,*end;} Cbor;
static int head(Cbor*c,uint8_t*type,uint64_t*v){
 if(c->p==c->end)return 0;uint8_t b=*c->p++;*type=b>>5;unsigned a=b&31;
 if(a<24){*v=a;return 1;}unsigned n=a==24?1:a==25?2:a==26?4:a==27?8:0;
 if(!n||(uint64_t)(c->end-c->p)<n)return 0;*v=0;for(unsigned i=0;i<n;i++)*v=(*v<<8)|*c->p++;
 return *v>=(n==1?24:n==2?256:n==4?65536:0x100000000ull);
}
static int item(Cbor*c,unsigned depth){
 if(depth>16)return 0;uint8_t t;uint64_t n;if(!head(c,&t,&n))return 0;
 if(t<2)return 1;if(t==2||t==3){if(!span(0,n,(uint64_t)(c->end-c->p))||(t==3&&!utf8(c->p,n)))return 0;c->p+=n;return 1;}
 if(t==7)return n==20||n==21||n==22;
 if(t!=4&&t!=5)return 0;if(n>65536)return 0;
 const uint8_t*prev=0;size_t plen=0;
 for(uint64_t i=0;i<n;i++){
  if(t==5){const uint8_t*start=c->p;uint8_t kt;uint64_t len;if(!head(c,&kt,&len)||kt!=3||!span(0,len,c->end-c->p)||!utf8(c->p,len))return 0;c->p+=len;size_t size=c->p-start;
   if(prev&&(size<plen||(size==plen&&memcmp(start,prev,size)<=0)))return 0;prev=start;plen=size;}
  if(!item(c,depth+1))return 0;
 }return 1;
}
int sf_manifest_text(const SfPackage*p,const char*key,char*out,size_t size){
 Cbor c={p->manifest,p->manifest+p->manifest_bytes};uint8_t t;uint64_t n;
 if(!head(&c,&t,&n)||t!=5)return SF_BAD_FORMAT;
 for(uint64_t i=0;i<n;i++){uint64_t len;if(!head(&c,&t,&len)||t!=3||!span(0,len,c.end-c.p))return SF_BAD_FORMAT;
  int match=len==strlen(key)&&!memcmp(c.p,key,len);c.p+=len;
  if(match){if(!head(&c,&t,&len)||t!=3||len>=size||!span(0,len,c.end-c.p))return SF_BAD_FORMAT;memcpy(out,c.p,len);out[len]=0;return SF_OK;}
  if(!item(&c,1))return SF_BAD_FORMAT;
 }return SF_NOT_FOUND;
}
int sf_manifest_capability(const SfPackage*p,const char*capability){
 Cbor c={p->manifest,p->manifest+p->manifest_bytes};uint8_t t;uint64_t n;
 if(!head(&c,&t,&n)||t!=5)return SF_BAD_FORMAT;
 for(uint64_t i=0;i<n;i++){uint64_t len;if(!head(&c,&t,&len)||t!=3||!span(0,len,c.end-c.p))return SF_BAD_FORMAT;
  int match=len==14&&!memcmp(c.p,"requested_caps",14);c.p+=len;
  if(match){uint64_t count;if(!head(&c,&t,&count)||t!=4||count>65536)return SF_BAD_FORMAT;int found=0;
   for(uint64_t j=0;j<count;j++){if(!head(&c,&t,&len)||t!=3||!span(0,len,c.end-c.p)||!utf8(c.p,len))return SF_BAD_FORMAT;if(len==strlen(capability)&&!memcmp(c.p,capability,len))found=1;c.p+=len;}return found;
  }
  if(!item(&c,1))return SF_BAD_FORMAT;
 }return 0;
}
static int app_abi(const SfPackage*p){
 Cbor c={p->manifest,p->manifest+p->manifest_bytes};uint8_t t;uint64_t n;
 if(!head(&c,&t,&n)||t!=5)return 0;
 for(uint64_t i=0;i<n;i++){uint64_t len;if(!head(&c,&t,&len)||t!=3||!span(0,len,c.end-c.p))return 0;
  int match=len==7&&!memcmp(c.p,"app_abi",7);c.p+=len;
  if(match){uint64_t abi;return head(&c,&t,&abi)&&t==0&&abi==SF_ABI;}
  if(!item(&c,1))return 0;
 }return 0;
}
static int manifest_value(const SfPackage*p,const char*key,Cbor*out){
 Cbor c={p->manifest,p->manifest+p->manifest_bytes};uint8_t t;uint64_t n;
 if(!head(&c,&t,&n)||t!=5)return SF_BAD_FORMAT;
 for(uint64_t i=0;i<n;i++){uint64_t len;if(!head(&c,&t,&len)||t!=3||!span(0,len,c.end-c.p))return SF_BAD_FORMAT;
  int match=len==strlen(key)&&!memcmp(c.p,key,len);c.p+=len;if(match){*out=c;return 0;}if(!item(&c,1))return SF_BAD_FORMAT;
 }return SF_NOT_FOUND;
}
int sf_native_relocations_validate(const SfPackage*p){
 if(!p||p->count>64||p->native_profile>SF_NATIVE_RELATIVE||p->relocation_count>SF_RELOCATION_MAX)return SF_BAD_FORMAT;
 if(p->native_profile==SF_NATIVE_STATIC)return p->relocation_count||p->relocation_offset?SF_BAD_FORMAT:0;
 if(p->kind!=2||!span(p->relocation_offset,p->relocation_count*16ull,p->bytes))return SF_BAD_FORMAT;
 uint64_t previous=0;
 for(unsigned i=0;i<p->relocation_count;i++){
  const uint8_t*r=p->data+p->relocation_offset+i*16;uint64_t target=sf_u64(r),addend=sf_u64(r+8);int write=0,value=0;
  if((target&7)||(i&&target<=previous)||addend>=SF_IMAGE_MAX)return SF_BAD_FORMAT;previous=target;
  for(unsigned j=0;j<p->count;j++){const SfSection*s=&p->sections[j];if(s->type>3)continue;
   if(s->type==3&&target>=s->rva&&span(target-s->rva,8,s->memory))write=1;
   if(addend>=s->rva&&addend-s->rva<s->memory)value=1;
  }
  if(!write||!value)return SF_BAD_FORMAT;
 }return 0;
}
static int native_profile(SfPackage*p){
 char profile[32];int r=sf_manifest_text(p,"native_profile",profile,sizeof(profile));Cbor c;
 if(r!=SF_NOT_FOUND&&(r||strcmp(profile,"static-v1"))){
  if(r||strcmp(profile,"relative-v1")||p->kind!=2)return SF_BAD_FORMAT;p->native_profile=SF_NATIVE_RELATIVE;
 }
 /* Recognized import metadata cannot silently become a relative-only image. */
 if(manifest_value(p,"import_relocations",&c)!=SF_NOT_FOUND||manifest_value(p,"native_imports",&c)!=SF_NOT_FOUND)return SF_BAD_FORMAT;
 r=manifest_value(p,"relative_relocations",&c);
 if(!p->native_profile){if(r!=SF_NOT_FOUND)return SF_BAD_FORMAT;for(unsigned i=0;i<p->count;i++)if(p->sections[i].type==4&&p->sections[i].bytes)return SF_BAD_FORMAT;return 0;}
 uint8_t t;uint64_t n,section=UINT64_MAX,offset=UINT64_MAX,bytes=UINT64_MAX;
 if(r||!head(&c,&t,&n)||t!=5||n!=3)return SF_BAD_FORMAT;
 for(unsigned i=0;i<3;i++){uint64_t len,v;if(!head(&c,&t,&len)||t!=3||!span(0,len,c.end-c.p))return SF_BAD_FORMAT;
  const uint8_t*key=c.p;c.p+=len;if(!head(&c,&t,&v)||t!=0)return SF_BAD_FORMAT;
  if(len==7&&!memcmp(key,"section",7))section=v;else if(len==6&&!memcmp(key,"offset",6))offset=v;else if(len==5&&!memcmp(key,"bytes",5))bytes=v;else return SF_BAD_FORMAT;
 }
 if(section>=p->count||offset||bytes>SF_RELOCATION_MAX*16ull||(bytes&15))return SF_BAD_FORMAT;
 const SfSection*s=&p->sections[section];if(s->type!=4||bytes!=s->bytes)return SF_BAD_FORMAT;
 for(unsigned i=0;i<p->count;i++)if(i!=section&&p->sections[i].type==4)return SF_BAD_FORMAT;
 p->relocation_offset=s->offset;p->relocation_count=bytes/16;return sf_native_relocations_validate(p);
}
/* Applications must declare bounded capabilities and a resource budget that
 * covers their mapped image. Module manifests keep their existing schema. */
static int app_manifest(const SfPackage*p){
 Cbor c;uint8_t t;uint64_t n;
 if(manifest_value(p,"requested_caps",&c))return 0;
 if(!head(&c,&t,&n)||t!=4||n>32)return 0;
 for(uint64_t i=0;i<n;i++){uint64_t len;if(!head(&c,&t,&len)||t!=3||!len||len>64||!span(0,len,c.end-c.p)||!utf8(c.p,len))return 0;c.p+=len;}
 if(manifest_value(p,"resource_limits",&c))return 0;
 if(!head(&c,&t,&n)||t!=5||!n||n>16)return 0;
 uint64_t memory=0;int declared=0;
 for(uint64_t i=0;i<n;i++){
  uint64_t len;if(!head(&c,&t,&len)||t!=3||!span(0,len,c.end-c.p))return 0;const uint8_t*key=c.p;c.p+=len;
  if(len==6&&!memcmp(key,"memory",6)){if(declared||!head(&c,&t,&memory)||t!=0)return 0;declared=1;}
  else if(!item(&c,1))return 0;
 }
 if(!declared)return 0;
 uint64_t mapped=0;
 for(unsigned i=0;i<p->count;i++)if(p->sections[i].type<=3){if(mapped>UINT64_MAX-p->sections[i].memory)return 0;mapped+=p->sections[i].memory;}
 return memory>=mapped&&memory<=SF_PACKAGE_MAX;
}
int sf_trust_validate(const SfTrustStore*trust){
 if(!trust||!trust->keys||!trust->count||trust->count>SF_TRUST_MAX)return SF_BAD_FORMAT;
 for(unsigned i=0;i<trust->count;i++){
  const SfTrustKey*k=&trust->keys[i];
  if(!k->usage||(k->usage&~(SF_TRUST_MODULE|SF_TRUST_APP))||k->revoked>1)return SF_BAD_FORMAT;
  for(unsigned j=0;j<i;j++)if(!memcmp(k->public_key,trust->keys[j].public_key,32))return SF_BAD_FORMAT;
 }return 0;
}
int sf_package_open_trusted(SfPackage*p,const void*data,uint64_t bytes,const SfTrustStore*trust){
 if(!p)return SF_INVALID;memset(p,0,sizeof(*p));
 int valid=sf_trust_validate(trust);if(valid)return valid;
 const uint8_t*d=data;if(!d)return SF_INVALID;
 if(bytes<264||bytes>SF_PACKAGE_MAX)return SF_BAD_FORMAT;
 int mod=!memcmp(d,"SFMOD1\0\0",8),app=!memcmp(d,"SFK1\r\n\032\n",8);
 if(!mod&&!app)return SF_BAD_FORMAT;
 if(sf_u16(d+8)!=1||sf_u16(d+10)||sf_u32(d+12)!=96||sf_u64(d+16)!=bytes||sf_u16(d+24)!=(mod?1:2)||sf_u16(d+26)!=1)return SF_BAD_FORMAT;
 if(sf_u16(d+28)!=1||sf_u16(d+30))return SF_ABI_MISMATCH;
 if(sf_u32(d+32)||sf_u32(d+76)||!zero(d+88,8)||sf_u32(d+72)!=104)return SF_BAD_FORMAT;
 uint32_t count=sf_u32(d+36);uint64_t table=sf_u64(d+40),manifest=sf_u64(d+48),mlen=sf_u64(d+56),sig=sf_u64(d+64);
 if(!count||count>64||!mlen||mlen>65536||!span(table,(uint64_t)count*64,bytes)||!span(manifest,mlen,bytes)||!span(sig,104,bytes)||sig+104!=bytes)return SF_BAD_FORMAT;
 uint64_t starts[68]={0,table,manifest,sig},lens[68]={96,count*64ull,mlen,104};unsigned ranges=4;
 p->data=d;p->bytes=bytes;p->manifest=d+manifest;p->manifest_bytes=mlen;p->count=count;p->kind=mod?1:2;p->entry=sf_u64(d+80);p->signature=sig;
 int entry_ok=0;unsigned mapped=0;
 for(unsigned i=0;i<count;i++){
  const uint8_t*s=d+table+i*64;SfSection*x=&p->sections[i];x->type=sf_u32(s);x->flags=sf_u32(s+4);x->offset=sf_u64(s+8);x->bytes=sf_u64(s+16);x->memory=sf_u64(s+24);x->rva=sf_u64(s+32);x->alignment=sf_u64(s+40);
  if(!zero(s+48,16)||!span(x->offset,x->bytes,sig)||x->type<1||x->type>5)return SF_BAD_FORMAT;
  if(x->bytes){starts[ranges]=x->offset;lens[ranges++]=x->bytes;}
  if(x->type<=3){mapped++;uint32_t flags=x->type==1?5:x->type==2?1:3;
   if(x->flags!=flags||x->alignment!=4096||(x->rva&4095)||(x->offset&4095)||!x->memory||x->bytes>x->memory||!span(x->rva,x->memory,SF_IMAGE_MAX))return SF_BAD_FORMAT;
   uint64_t end=(x->rva+x->memory+4095)&~4095ull;
   for(unsigned j=0;j<i;j++){SfSection*y=&p->sections[j];if(y->type<=3&&x->rva<((y->rva+y->memory+4095)&~4095ull)&&y->rva<end)return SF_BAD_FORMAT;}
   if(x->type==1&&p->entry>=x->rva&&p->entry-x->rva<x->bytes)entry_ok=1;
  }else if(x->flags||x->rva||x->alignment!=8||(x->offset&7)||x->bytes!=x->memory)return SF_BAD_FORMAT;
 }
 if(!entry_ok||mapped>8)return SF_BAD_FORMAT;
 for(unsigned i=0;i<ranges;i++)for(unsigned j=i+1;j<ranges;j++)if(starts[j]<starts[i]){uint64_t t=starts[i];starts[i]=starts[j];starts[j]=t;t=lens[i];lens[i]=lens[j];lens[j]=t;}
 uint64_t end=0;for(unsigned i=0;i<ranges;i++){if(starts[i]<end||!zero(d+end,starts[i]-end))return SF_BAD_FORMAT;end=starts[i]+lens[i];}
 if(end!=bytes)return SF_BAD_FORMAT;
 Cbor c={p->manifest,p->manifest+mlen};if((*c.p>>5)!=5||!item(&c,0)||c.p!=c.end)return SF_BAD_FORMAT;
 char text[96];if(sf_manifest_text(p,mod?"module_id":"app_id",text,sizeof(text))||!text[0]||sf_manifest_text(p,"version",text,sizeof(text)))return SF_BAD_FORMAT;
 if(app&&sf_manifest_text(p,"runtime_id",text,sizeof(text)))return SF_BAD_FORMAT;
 if(app&&!app_abi(p))return SF_ABI_MISMATCH;
 if(app&&!app_manifest(p))return SF_BAD_FORMAT;
 if(sf_u16(d+sig+32)!=1||sf_u16(d+sig+34)||sf_u32(d+sig+36)!=64)return SF_BAD_FORMAT;
 const SfTrustKey*key=0;uint8_t hash[32],message[55];
 for(unsigned i=0;i<trust->count;i++){sf_sha256(hash,trust->keys[i].public_key,32);if(!memcmp(hash,d+sig,32))key=&trust->keys[i];}
 if(!key)return SF_BAD_SIGNATURE;
 if(key->revoked)return SF_REVOKED;
 if(!(key->usage&(mod?SF_TRUST_MODULE:SF_TRUST_APP)))return SF_DENIED;
 static const uint8_t domain[]="SpiritFoxOS-Package-v1";
 _Static_assert(sizeof(domain)==23,"signature domain");memcpy(message,domain,sizeof(domain));sf_sha256(message+sizeof(domain),d,bytes-64);
 if(crypto_ed25519_check(d+bytes-64,key->public_key,message,sizeof(message)))return SF_BAD_SIGNATURE;
 int r=native_profile(p);if(!r)p->key_id=d+sig;return r;
}
/* Explicit-key verification is retained for offline authoring tools. Runtime
 * consumers use the complete policy and must never fall back to this API. */
int sf_package_open(SfPackage*p,const void*data,uint64_t bytes,const uint8_t pub[32]){
 if(!pub){if(p)memset(p,0,sizeof(*p));return SF_INVALID;}
 SfTrustKey key={{0},SF_TRUST_MODULE|SF_TRUST_APP,0};memcpy(key.public_key,pub,32);
 SfTrustStore trust={&key,1};return sf_package_open_trusted(p,data,bytes,&trust);
}
/* Explicit anchor sets are an offline compatibility API, never a runtime
 * fallback. The runtime always retains immutable usage/revocation rules. */
int sf_package_open_set(SfPackage*p,const void*data,uint64_t bytes,const uint8_t (*keys)[32],unsigned count){
 if(!keys||!count||count>SF_TRUST_MAX){if(p)memset(p,0,sizeof(*p));return SF_INVALID;}
 SfTrustKey entries[SF_TRUST_MAX];memset(entries,0,sizeof(entries));
 for(unsigned i=0;i<count;i++){memcpy(entries[i].public_key,keys[i],32);entries[i].usage=SF_TRUST_MODULE|SF_TRUST_APP;}
 SfTrustStore policy={entries,count};return sf_package_open_trusted(p,data,bytes,&policy);
}
int sf_bundle_find(const void*data,uint64_t bytes,const char*name,const uint8_t**out,uint64_t*length){
 const uint8_t*d=data;if(bytes<16||memcmp(d,"SFB1",4)||sf_u32(d+4)>32||sf_u64(d+8)!=bytes)return SF_BAD_FORMAT;uint32_t n=sf_u32(d+4);
 if(!span(16,(uint64_t)n*48,bytes))return SF_BAD_FORMAT;
 for(uint32_t i=0;i<n;i++){const uint8_t*e=d+16+i*48;size_t len=0;while(len<32&&e[len])len++;if(len==32)return SF_BAD_FORMAT;uint64_t off=sf_u64(e+32),size=sf_u64(e+40);
  if(off<16+n*48ull||!span(off,size,bytes))return SF_BAD_FORMAT;
  if(strlen(name)==len&&!memcmp(e,name,len)){*out=d+off;*length=size;return SF_OK;}}
 return SF_NOT_FOUND;
}
