#include <stdio.h>
#include <string.h>
#include "linux_image.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("LINUX ELF FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define FIX 0x12000u
static uint8_t image[FIX];
static size_t image_bytes;
static void w16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void w32(uint8_t*p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void w64(uint8_t*p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8));}
static uint64_t r64(const uint8_t*p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(i*8);return v;}
static uint8_t *phdr(unsigned i){return image+64+56*i;}
static void set_ph(unsigned i,uint32_t type,uint32_t flags,uint64_t off,uint64_t va,uint64_t file,uint64_t mem,uint64_t align){
 uint8_t*p=phdr(i);memset(p,0,56);
 w32(p,type);w32(p+4,flags);w64(p+8,off);w64(p+16,va);
 w64(p+32,file);w64(p+40,mem);w64(p+48,align);
}
/* 最小 ET_DYN 夹具：RX 首页含 ELF 头与代码，R 数据页，RW 数据页含 .dynamic、
 * 一个 RELATIVE 槽与 RELA 表；BSS 从 0x2200 到 0x3000。 */
static void build(void){
 memset(image,0,sizeof(image));
 image[0]=0x7f;image[1]='E';image[2]='L';image[3]='F';image[4]=2;image[5]=1;image[6]=1;
 w16(image+16,3);w16(image+18,62);w32(image+20,1);w64(image+24,0x200);w64(image+32,64);
 w16(image+52,64);w16(image+54,56);w16(image+56,4);w16(image+58,64);
 set_ph(0,1,5,0,0,0x1000,0x1000,0x1000);
 set_ph(1,1,4,0x1000,0x1000,0x100,0x100,0x1000);
 set_ph(2,1,6,0x2000,0x2000,0x200,0x300,0x1000);
 set_ph(3,2,6,0x2000,0x2000,64,64,8);
 image[0x200]=0x0f;image[0x201]=0x05;image[0x202]=0xc3;
 image[0x1000]=0xaa;
 w64(image+0x2000,7);w64(image+0x2008,0x2100);
 w64(image+0x2010,8);w64(image+0x2018,24);
 w64(image+0x2020,9);w64(image+0x2028,24);
 image[0x2080]=0x11;image[0x2081]=0x11;
 image[0x2090]=0x0f;image[0x2091]=0x05;
 w64(image+0x2100,0x2080);w64(image+0x2108,8);w64(image+0x2110,0x2110);
 image_bytes=0x2200;
}
static int plan_ok(const SfLoadPlan*p){
 if(p->size!=sizeof(*p)||!p->count||p->count>16||!sf_image_base_valid(p->image_base))return 0;
 int entry=0;
 for(unsigned i=0;i<p->count;i++){const SfLoadSegment*s=&p->segments[i];
  if((s->rva&4095)||!s->bytes||s->rva>=SF_IMAGE_MAX||s->bytes>SF_IMAGE_MAX-s->rva)return 0;
  if(s->flags!=1&&s->flags!=3&&s->flags!=5)return 0;
  uint64_t end=(s->rva+s->bytes+4095)&~4095ull;
  for(unsigned j=0;j<i;j++){const SfLoadSegment*o=&p->segments[j];
   uint64_t oend=(o->rva+o->bytes+4095)&~4095ull;
   if(s->rva<oend&&o->rva<end)return 0;
  }
  if(s->flags==5&&p->entry>=s->rva&&p->entry-s->rva<s->bytes)entry=1;
 }
 return entry;
}
static void reset(uint32_t type,uint32_t flags){build();set_ph(3,type,flags,0x2000,0x2000,64,64,8);}
/* ---- S5 多对象动态夹具 ---- */
static uint8_t libimg[0x1000];
static void set_ph_at(uint8_t*img,unsigned i,uint32_t type,uint32_t flags,uint64_t off,uint64_t va,uint64_t file,uint64_t mem,uint64_t align){
 uint8_t*p=img+64+56*i;memset(p,0,56);w32(p,type);w32(p+4,flags);
 w64(p+8,off);w64(p+16,va);w64(p+32,file);w64(p+40,mem);w64(p+48,align);
}
static void set_sym(uint8_t*img,uint64_t off,uint32_t name,uint8_t info,uint16_t shndx,uint64_t value,uint64_t size){
 memset(img+off,0,24);w32(img+off,name);img[off+4]=info;w16(img+off+6,shndx);w64(img+off+8,value);w64(img+off+16,size);
}
static void set_hash(uint8_t*img,uint64_t off,uint32_t nchain){
 memset(img+off,0,8+4+4*nchain);w32(img+off,1);w32(img+off+4,nchain);w32(img+off+8,nchain>1?1:0);
}
static void build_exe(void){
 memset(image,0,sizeof(image));
 image[0]=0x7f;image[1]='E';image[2]='L';image[3]='F';image[4]=2;image[5]=1;image[6]=1;
 w16(image+16,3);w16(image+18,62);w32(image+20,1);w64(image+24,0x200);w64(image+32,64);
 w16(image+52,64);w16(image+54,56);w16(image+56,5);w16(image+58,64);
 set_ph(0,1,5,0,0,0x1000,0x1000,0x1000);
 set_ph(1,1,4,0x1000,0x1000,0x1000,0x1000,0x1000);
 set_ph(2,1,6,0x2000,0x2000,0x1000,0x1000,0x1000);
 set_ph(3,2,6,0x2000,0x2000,0x100,0x100,8);
 set_ph(4,7,4,0x1040,0x1040,4,8,4);
 image[0x200]=0xc3;
 memcpy(image+0x1000,"\0liba.so\0liba_fn\0liba_tls\0",27);
 set_sym(image,0x1100,0,0,0,0,0);
 set_sym(image,0x1118,9,(1<<4)|2,0,0,0);
 set_sym(image,0x1130,17,(1<<4)|6,0,0,0);
 set_hash(image,0x1180,3);
 static const struct{uint64_t tag,val;} dyn[]={
  {1,1},{5,0x1000},{10,27},{6,0x1100},{11,24},{4,0x1180},
  {7,0x2100},{8,96},{9,24},{23,0x2160},{2,24},{20,7},{0,0}};
 for(unsigned i=0;i<sizeof(dyn)/sizeof(dyn[0]);i++){w64(image+0x2000+i*16,dyn[i].tag);w64(image+0x2008+i*16,dyn[i].val);}
 w64(image+0x2100,0x2180);w64(image+0x2108,(0ull<<32)|8);w64(image+0x2110,0x21a0);
 w64(image+0x2118,0x2188);w64(image+0x2120,(2ull<<32)|18);w64(image+0x2128,0);
 w64(image+0x2130,0x2190);w64(image+0x2138,(2ull<<32)|16);w64(image+0x2140,0);
 w64(image+0x2148,0x2198);w64(image+0x2150,(2ull<<32)|17);w64(image+0x2158,0);
 w64(image+0x2160,0x21a0);w64(image+0x2168,(1ull<<32)|7);w64(image+0x2170,0);
 image_bytes=0x3000;
}
static void build_lib(void){
 memset(libimg,0,sizeof(libimg));
 libimg[0]=0x7f;libimg[1]='E';libimg[2]='L';libimg[3]='F';libimg[4]=2;libimg[5]=1;libimg[6]=1;
 w16(libimg+16,3);w16(libimg+18,62);w32(libimg+20,1);w64(libimg+24,0x100);w64(libimg+32,64);
 w16(libimg+52,64);w16(libimg+54,56);w16(libimg+56,3);w16(libimg+58,64);
 set_ph_at(libimg,0,1,5,0,0,0x1000,0x1000,0x1000);
 set_ph_at(libimg,1,2,4,0x300,0x300,0x80,0x80,8);
 set_ph_at(libimg,2,7,4,0x900,0x900,4,8,4);
 libimg[0x100]=0xc3;
 memcpy(libimg+0x180,"\0liba.so\0liba_fn\0liba_tls\0",27);
 set_sym(libimg,0x200,0,0,0,0,0);
 set_sym(libimg,0x218,9,(1<<4)|2,1,0x100,0);
 set_sym(libimg,0x230,17,(1<<4)|6,1,0,0);
 set_hash(libimg,0x280,3);
 static const struct{uint64_t tag,val;} dyn[]={{14,1},{5,0x180},{10,27},{6,0x200},{11,24},{4,0x280},{0,0}};
 for(unsigned i=0;i<sizeof(dyn)/sizeof(dyn[0]);i++){w64(libimg+0x300+i*16,dyn[i].tag);w64(libimg+0x308+i*16,dyn[i].val);}
}
static int expect_validate(const char*what,int want){
 SfLinuxInfo info;int r=sf_linux_elf_validate(image,image_bytes,&info);
 checks++;
 if(r!=want){printf("LINUX ELF FAIL %s: got %d want %d\n",what,r,want);return 1;}
 return 0;
}
int main(void){
 static uint8_t staging[SF_IMAGE_MAX],copy[SF_IMAGE_MAX];
 SfLinuxInfo info;SfLoadPlan plan;
 build();
 CHECK(!sf_linux_elf_validate(image,image_bytes,&info));
 CHECK(info.entry_rva==0x200&&info.image_bytes==0x3000&&info.phdr_rva==64);
 CHECK(info.phnum==4&&info.relocations==1&&info.syscalls==0);
 CHECK(sf_linux_elf_validate(image,image_bytes-1,&info)==SF_BAD_FORMAT);
 CHECK(!sf_linux_elf_load(image,image_bytes,0x44000000,staging,sizeof(staging),&plan,&info,SF_LINUX_FLAG_REWRITE_SYSCALLS));
 CHECK(plan.size==sizeof(SfLoadPlan)&&plan.count==3&&plan.entry==0x200&&plan.image_base==0x44000000);
 CHECK(plan_ok(&plan));
 CHECK(plan.segments[0].rva==0&&plan.segments[0].bytes==0x1000&&plan.segments[0].flags==5);
 CHECK(plan.segments[1].rva==0x1000&&plan.segments[1].bytes==0x1000&&plan.segments[1].flags==1);
 CHECK(plan.segments[2].rva==0x2000&&plan.segments[2].bytes==0x1000&&plan.segments[2].flags==3);
 CHECK(((uint8_t*)plan.segments[0].data)[0x200]==0xcd&&((uint8_t*)plan.segments[0].data)[0x201]==0x80);
 CHECK(((uint8_t*)plan.segments[0].data)[0x202]==0xc3);
 CHECK(((uint8_t*)plan.segments[1].data)[0]==0xaa);
 CHECK(((uint8_t*)plan.segments[1].data)[0x100]==0);
 CHECK(r64((uint8_t*)plan.segments[2].data+0x80)==0x44000000ull+0x2110);
 CHECK(((uint8_t*)plan.segments[2].data)[0x90]==0x0f&&((uint8_t*)plan.segments[2].data)[0x91]==0x05);
 for(unsigned i=0x200;i<0x1000;i++)CHECK(((uint8_t*)plan.segments[2].data)[i]==0);
 CHECK(info.syscalls==1);
 memcpy(copy,staging,0x3000);
 CHECK(!sf_linux_elf_load(image,image_bytes,0x44000000,staging,sizeof(staging),&plan,&info,0));
 CHECK(info.syscalls==0&&((uint8_t*)plan.segments[0].data)[0x200]==0x0f);
 CHECK(!memcmp(copy+0x1000,staging+0x1000,0x2000)&&memcmp(copy,staging,0x1000));
 /* 基址、容量与空指针边界。 */
 CHECK(sf_linux_elf_load(image,image_bytes,0,staging,sizeof(staging),&plan,&info,0)==SF_INVALID);
 CHECK(sf_linux_elf_load(image,image_bytes,SF_BASE,staging,sizeof(staging),&plan,&info,0)==SF_INVALID);
 CHECK(sf_linux_elf_load(image,image_bytes,0x44000001,staging,sizeof(staging),&plan,&info,0)==SF_INVALID);
 CHECK(sf_linux_elf_load(image,image_bytes,0x44000000,staging,0x2fff,&plan,&info,0)==SF_NO_SPACE);
 CHECK(sf_linux_elf_load(image,image_bytes,0x44000000,0,sizeof(staging),&plan,&info,0)==SF_INVALID);
 /* 头部与段表负例。 */
 build();image[1]=0;CHECK(!expect_validate("magic",SF_BAD_FORMAT));
 build();image[4]=1;CHECK(!expect_validate("class",SF_BAD_FORMAT));
 build();image[5]=2;CHECK(!expect_validate("endian",SF_BAD_FORMAT));
 build();w16(image+16,2);CHECK(!expect_validate("et_exec",SF_BAD_FORMAT));
 build();w16(image+18,3);CHECK(!expect_validate("machine",SF_BAD_FORMAT));
 build();w16(image+54,64);CHECK(!expect_validate("phentsize",SF_BAD_FORMAT));
 build();w16(image+56,0);CHECK(!expect_validate("phnum",SF_BAD_FORMAT));
 build();w64(image+32,0x4000);CHECK(!expect_validate("phoff",SF_BAD_FORMAT));
 build();w64(image+24,0x1000);CHECK(!expect_validate("entry_not_rx",SF_BAD_FORMAT));
 build();w64(image+24,0x900000);CHECK(!expect_validate("entry_high",SF_BAD_FORMAT));
 /* PT_LOAD 负例。 */
 build();w64(phdr(0)+32,0x1001);CHECK(!expect_validate("filesz_gt_memsz",SF_BAD_FORMAT));
 build();w32(phdr(0)+4,7);CHECK(!expect_validate("rwx",SF_BAD_FORMAT));
 build();w32(phdr(0)+4,2);CHECK(!expect_validate("write_only",SF_BAD_FORMAT));
 build();w64(phdr(0)+16,0x100);CHECK(!expect_validate("va_align",SF_BAD_FORMAT));
 build();w64(phdr(2)+32,0x4000);CHECK(!expect_validate("file_bounds",SF_BAD_FORMAT));
 build();w64(phdr(2)+40,0x801000);CHECK(!expect_validate("image_limit",SF_NO_SPACE));
 build();set_ph(1,1,4,0,0,0x100,0x100,0x1000);CHECK(!expect_validate("overlap",SF_BAD_FORMAT));
 /* 十七个不同权限页 → 合并后超过 16 段。 */
 memset(image,0,sizeof(image));
 image[0]=0x7f;image[1]='E';image[2]='L';image[3]='F';image[4]=2;image[5]=1;image[6]=1;
 w16(image+16,3);w16(image+18,62);w32(image+20,1);w64(image+24,0x400);w64(image+32,64);
 w16(image+52,64);w16(image+54,56);w16(image+56,17);w16(image+58,64);
 for(unsigned i=0;i<17;i++)set_ph(i,1,i?(i&1?6:4):5,i*0x1000,i*0x1000,i?0x100:0x1000,i?0x100:0x1000,0x1000);
 image[0x400]=0xc3;image_bytes=0x11000;
 CHECK(!expect_validate("segments_validate",SF_OK));
 CHECK(sf_linux_elf_load(image,image_bytes,0x44000000,staging,sizeof(staging),&plan,&info,0)==SF_NO_SPACE);
 /* 动态表与重定位负例。 */
 reset(3,6);memcpy(image+0x2000,"/lib/ld-musl-x86_64.so.1",25);
 CHECK(!expect_validate("interp",SF_OK));
 reset(7,6);CHECK(!expect_validate("tls",SF_OK));
 build();w64(image+0x2000,1);w64(image+0x2008,1);CHECK(!expect_validate("needed",SF_BAD_FORMAT));
 build();w64(image+0x2020,17);w64(image+0x2028,24);CHECK(!expect_validate("rel",SF_BAD_FORMAT));
 build();w64(image+0x2020,36);w64(image+0x2028,8);CHECK(!expect_validate("relr",SF_BAD_FORMAT));
 build();w64(image+0x2000,30);w64(image+0x2008,4);CHECK(!expect_validate("textrel_flag",SF_BAD_FORMAT));
 build();w64(image+0x2000,22);w64(image+0x2008,0);CHECK(!expect_validate("textrel_tag",SF_BAD_FORMAT));
 build();w64(image+0x2100,0x200);CHECK(!expect_validate("reloc_target_rx",SF_BAD_FORMAT));
 build();w64(image+0x2100,0x2084);CHECK(!expect_validate("reloc_misaligned",SF_BAD_FORMAT));
 build();w64(image+0x2108,9);CHECK(!expect_validate("reloc_type",SF_BAD_FORMAT));
 build();w64(image+0x2110,0x9000);CHECK(!expect_validate("reloc_addend_high",SF_BAD_FORMAT));
 build();w64(image+0x2110,(uint64_t)-1);CHECK(!expect_validate("reloc_addend_negative",SF_BAD_FORMAT));
 build();w64(image+0x2100,0x2200);CHECK(!expect_validate("reloc_bss_aligned",SF_OK));
 /* 相邻同权限段合并：把 PT_DYNAMIC 改成相邻 RW 页。 */
 build();set_ph(3,1,6,0x3000,0x3000,0x100,0x100,0x1000);image_bytes=0x3100;
 CHECK(!sf_linux_elf_validate(image,image_bytes,&info)&&info.relocations==0);
 CHECK(!sf_linux_elf_load(image,image_bytes,0x44000000,staging,sizeof(staging),&plan,&info,0));
 CHECK(plan.count==3&&plan.segments[2].rva==0x2000&&plan.segments[2].bytes==0x2000);
 CHECK(plan_ok(&plan));
 /* 非页对齐 p_vaddr/p_offset（页内偏移 0x2a0）与不相容偏移。 */
 build();image[0x12a0]=0x5a;
 set_ph(1,1,5,0x12a0,0x12a0,0x10,0x10,0x1000);
 CHECK(!sf_linux_elf_validate(image,image_bytes,&info));
 CHECK(!sf_linux_elf_load(image,image_bytes,0x44000000,staging,sizeof(staging),&plan,&info,0));
 CHECK(plan.count==2&&plan.segments[0].bytes==0x2000&&((uint8_t*)plan.segments[0].data)[0x12a0]==0x5a);
 CHECK(plan_ok(&plan));
 build();set_ph(1,1,5,0x1000,0x12a0,0x10,0x10,0x1000);
 CHECK(!expect_validate("offset_congruence",SF_BAD_FORMAT));
 /* 初始栈布局。 */
 build();CHECK(!sf_linux_elf_validate(image,image_bytes,&info));
 static const char *args[]={"hello","--flag"};
 static const char *envs[]={"PATH=/bin","MODE=demo"};
 uint8_t random16[16];for(unsigned i=0;i<16;i++)random16[i]=(uint8_t)(i*7+1);
 SfLinuxStackRequest req;memset(&req,0,sizeof(req));
 req.argv=args;req.argc=2;req.env=envs;req.envc=2;req.uid=1000;req.gid=1001;req.random16=random16;
 SfLinuxStack stack;uint64_t top=0x41010000;
 CHECK(!sf_linux_stack_init(staging,0x2000,top,0x44000000,&info,&req,&stack));
 CHECK((stack.sp&15)==0&&stack.sp>=top-0x2000&&stack.bytes<=0x2000);
 CHECK(stack.argc==2&&stack.argv==stack.sp+8);
 uint64_t low=top-0x2000;uint8_t*base=staging;
 CHECK(r64(base+(stack.sp-low))==2);
 uint64_t a0=r64(base+(stack.argv-low)),a1=r64(base+(stack.argv+8-low)),a2=r64(base+(stack.argv+16-low));
 CHECK(!a2&&!strcmp((char*)base+(a0-low),"hello")&&!strcmp((char*)base+(a1-low),"--flag"));
 uint64_t e0=r64(base+(stack.envp-low)),e1=r64(base+(stack.envp+8-low)),e2=r64(base+(stack.envp+16-low));
 CHECK(!e2&&!strcmp((char*)base+(e0-low),"PATH=/bin")&&!strcmp((char*)base+(e1-low),"MODE=demo"));
 int seen_phdr=0,seen_entry=0,seen_page=0,seen_random=0,seen_null=0;
 for(unsigned i=0;i<16;i++){uint64_t tag=r64(base+(stack.auxv-low)+i*16),val=r64(base+(stack.auxv-low)+i*16+8);
  if(!tag){seen_null=1;break;}
  if(tag==3){CHECK(val==0x44000000ull+64);seen_phdr=1;}
  if(tag==9){CHECK(val==0x44000000ull+0x200);seen_entry=1;}
  if(tag==6){CHECK(val==SF_PAGE);seen_page=1;}
  if(tag==25){CHECK(val>=low&&val+16<=top);seen_random=1;}
 }
 CHECK(seen_phdr&&seen_entry&&seen_page&&seen_random&&seen_null);
 /* 栈负例。 */
 memset(&req,0,sizeof(req));req.argv=args;req.argc=0;
 CHECK(sf_linux_stack_init(staging,0x2000,top,0x44000000,&info,&req,&stack)==SF_BAD_FORMAT);
 req.argc=SF_LINUX_ARG_MAX+1;CHECK(sf_linux_stack_init(staging,0x2000,top,0x44000000,&info,&req,&stack)==SF_BAD_FORMAT);
 req.argc=1;req.argv=0;CHECK(sf_linux_stack_init(staging,0x2000,top,0x44000000,&info,&req,&stack)==SF_BAD_FORMAT);
 req.argv=args;CHECK(sf_linux_stack_init(staging,0x20,top,0x44000000,&info,&req,&stack)==SF_NO_SPACE);
 CHECK(sf_linux_stack_init(staging,0x2000,0x1000,0x44000000,&info,&req,&stack)==SF_INVALID);
 CHECK(sf_linux_stack_init(0,0x2000,top,0x44000000,&info,&req,&stack)==SF_INVALID);
 SfLinuxInfo nophdr=info;nophdr.phdr_rva=0;
 CHECK(sf_linux_stack_init(staging,0x2000,top,0x44000000,&nophdr,&req,&stack)==SF_BAD_FORMAT);
 static char huge[SF_LINUX_STRING_MAX+2];memset(huge,'x',sizeof(huge));huge[sizeof(huge)-1]=0;
 static const char *huge_args[]={huge};
 req.argv=huge_args;CHECK(sf_linux_stack_init(staging,0x2000,top,0x44000000,&info,&req,&stack)==SF_BAD_FORMAT);
 /* S5 多对象动态链接：主程序 + liba.so，符号/重定位/TLS/PT_TLS 回填。 */
 static uint8_t staging2[SF_IMAGE_MAX];
 build_exe();build_lib();
 SfLinuxObject objs[2]={{0,image,image_bytes,0},{0,libimg,sizeof(libimg),0}};
 CHECK(sf_linux_elf_validate(image,image_bytes,&info)==SF_DEPENDENCY);
 CHECK(!sf_linux_elf_validate(libimg,sizeof(libimg),&info));
 CHECK(!sf_linux_image_load(objs,2,0x44000000,staging2,sizeof(staging2),&plan,&info,0));
 CHECK(info.object_count==2&&info.tls_size==16&&info.tls_align==4&&plan.entry==0x200);
 CHECK(plan_ok(&plan));
 CHECK(r64((uint8_t*)plan.segments[2].data+0x1a0)==0x44000000ull+0x3100);
 CHECK(r64((uint8_t*)plan.segments[2].data+0x180)==0x44000000ull+0x21a0);
 CHECK(r64((uint8_t*)plan.segments[2].data+0x188)==(uint64_t)-8);
 CHECK(r64((uint8_t*)plan.segments[2].data+0x190)==2);
 CHECK(r64((uint8_t*)plan.segments[2].data+0x198)==8);
 CHECK(r64((uint8_t*)plan.segments[0].data+0x130)==0x4000);
 CHECK(sf_linux_image_load(objs,1,0x44000000,staging2,sizeof(staging2),&plan,&info,0)==SF_DEPENDENCY);
 build_lib();w32(libimg+0x218,25);
 CHECK(sf_linux_image_load(objs,2,0x44000000,staging2,sizeof(staging2),&plan,&info,0)==SF_DEPENDENCY);
 build_lib();static const uint8_t zero_digest[32]={0};objs[0].digest32=zero_digest;
 CHECK(sf_linux_image_load(objs,2,0x44000000,staging2,sizeof(staging2),&plan,&info,0)==SF_BAD_FORMAT);
 objs[0].digest32=0;
 build();
 /* 固定种子变异：任何被接受的映像都必须能构造合法计划；失败必须显式返回。 */
 uint64_t seed=0x123456789abcdefull;unsigned accepted=0,rejected=0;
 for(unsigned n=0;n<2000;n++){
  memcpy(copy,image,image_bytes);
  seed=seed*6364136223846793005ull+1442695040888963407ull;
  unsigned pos=(unsigned)((seed>>17)%image_bytes);
  unsigned bit=(unsigned)((seed>>9)&7);
  copy[pos]^=(uint8_t)(1u<<bit);
  int r=sf_linux_elf_validate(copy,image_bytes,&info);
  if(r){rejected++;continue;}
  accepted++;
  r=sf_linux_elf_load(copy,image_bytes,0x44000000,staging,sizeof(staging),&plan,&info,SF_LINUX_FLAG_REWRITE_SYSCALLS);
  CHECK(!r&&plan_ok(&plan));
 }
 printf("LINUX ELF PASS: %u checks; segments/relocations/stack/mutations accepted=%u rejected=%u\n",checks,accepted,rejected);
 return 0;
}
