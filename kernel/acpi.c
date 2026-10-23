/* Bounded ACPI discovery. Unsupported DMAR layouts fail closed. */
#include "iommu.h"
static uint16_t u16(const uint8_t*p){uint16_t x;memcpy(&x,p,2);return x;}
static uint32_t u32(const uint8_t*p){uint32_t x;memcpy(&x,p,4);return x;}
static uint64_t u64(const uint8_t*p){uint64_t x;memcpy(&x,p,8);return x;}
static int checksum(SfPhysRead read,void*c,uint64_t address,uint32_t bytes){
 if(address>UINT64_MAX-bytes)return 0;uint8_t block[256],sum=0;
 for(unsigned off=0;off<bytes;){unsigned n=bytes-off;if(n>sizeof(block))n=sizeof(block);if(read(c,address+off,block,n))return 0;for(unsigned i=0;i<n;i++)sum+=block[i];off+=n;}return !sum;
}
static int table(SfPhysRead read,void*c,uint64_t address,const char*magic,uint32_t*bytes){
 uint8_t h[36];if(read(c,address,h,sizeof(h))||memcmp(h,magic,4))return SF_BAD_FORMAT;
 *bytes=u32(h+4);return *bytes>=36&&*bytes<=4096&&checksum(read,c,address,*bytes)?0:SF_BAD_FORMAT;
}
int sf_acpi_dmar(SfPhysRead read,void*c,uint64_t rsdp,SfDmarInfo*out){
 memset(out,0,sizeof(*out));if(!rsdp)return SF_NOT_FOUND;uint8_t r[36];if(read(c,rsdp,r,20)||memcmp(r,"RSD PTR ",8)||!checksum(read,c,rsdp,20))return SF_BAD_FORMAT;
 uint64_t root=u32(r+16);unsigned stride=4;uint32_t bytes;
 if(r[15]>=2){if(read(c,rsdp,r,36)||u32(r+20)<36||u32(r+20)>4096||!checksum(read,c,rsdp,u32(r+20)))return SF_BAD_FORMAT;if(u64(r+24)){root=u64(r+24);stride=8;}}
 if(table(read,c,root,stride==8?"XSDT":"RSDT",&bytes)||(bytes-36)%stride)return SF_BAD_FORMAT;
 uint64_t dmar=0;for(unsigned off=36;off<bytes;off+=stride){uint8_t e[8]={0},sig[4];if(read(c,root+off,e,stride))return SF_BAD_FORMAT;uint64_t address=stride==8?u64(e):u32(e);if(read(c,address,sig,4))return SF_BAD_FORMAT;if(!memcmp(sig,"DMAR",4)){if(dmar)return SF_BAD_FORMAT;dmar=address;}}
 if(!dmar)return SF_NOT_FOUND;if(table(read,c,dmar,"DMAR",&bytes)||bytes<48)return SF_BAD_FORMAT;
 uint8_t h[48];if(read(c,dmar,h,48)||h[36]<38||h[36]>63)return SF_BAD_FORMAT;for(unsigned i=38;i<48;i++)if(h[i])return SF_BAD_FORMAT;
 unsigned units=0;for(unsigned off=48;off<bytes;){uint8_t e[16];if(bytes-off<4||read(c,dmar+off,e,4))return SF_BAD_FORMAT;unsigned type=u16(e),n=u16(e+2);if(n<4||n>bytes-off)return SF_BAD_FORMAT;
  /* One DRHD, segment zero; include-all or explicit bus-zero endpoint scopes. */
  if(type!=0||n<16||read(c,dmar+off,e,16)||e[4]>1||e[5]||u16(e+6)||u64(e+8)!=SF_VTD_MMIO||++units!=1)return SF_DENIED;
  if(e[4])memset(out->covered,1,sizeof(out->covered));
  for(unsigned pos=16;pos<n;){uint8_t scope[8];if(n-pos<8||read(c,dmar+off+pos,scope,8)||!scope[0]||scope[0]>5||scope[1]<8||scope[1]>n-pos||((scope[1]-6)&1)||scope[2]||scope[3])return SF_BAD_FORMAT;
   if(scope[0]==1){if(scope[1]!=8||scope[5]||scope[6]>31||scope[7]>7)return SF_DENIED;out->covered[scope[6]*8+scope[7]]=1;}pos+=scope[1];}
  off+=n;
 }
 if(units!=1)return SF_BAD_FORMAT;out->base=SF_VTD_MMIO;return 0;
}
