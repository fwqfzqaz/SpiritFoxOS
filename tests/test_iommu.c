#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iommu.h"
static uint8_t memory[0x5000],saved[0x5000];
static SfVtdTables tables;
static int checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"IOMMU FAIL line %d\n",__LINE__);exit(1);}}while(0)
static void put16(unsigned a,uint16_t n){memcpy(memory+a,&n,2);}
static void put32(unsigned a,uint32_t n){memcpy(memory+a,&n,4);}
static void put64(unsigned a,uint64_t n){memcpy(memory+a,&n,8);}
static void seal(unsigned a,unsigned n,unsigned field){memory[a+field]=0;uint8_t sum=0;for(unsigned i=0;i<n;i++)sum+=memory[a+i];memory[a+field]=0-sum;}
static int read_memory(void*c,uint64_t address,void*out,size_t bytes){if(address>sizeof(memory)||bytes>sizeof(memory)-address)return SF_BAD_FORMAT;memcpy(out,memory+address,bytes);return 0;}
static void fixture(void){
 memset(memory,0,sizeof(memory));memcpy(memory+0x1000,"RSD PTR ",8);memory[0x100f]=2;put32(0x1014,36);put64(0x1018,0x2000);seal(0x1000,20,8);seal(0x1000,36,32);
 memcpy(memory+0x2000,"XSDT",4);put32(0x2004,44);put64(0x2024,0x3000);seal(0x2000,44,9);
 memcpy(memory+0x3000,"DMAR",4);put32(0x3004,80);memory[0x3024]=38;put16(0x3032,32);put64(0x3038,SF_VTD_MMIO);
 uint8_t scopes[]={3,8,0,0,0,0xf0,31,0,1,8,0,0,0,0,5,0};memcpy(memory+0x3040,scopes,sizeof(scopes));seal(0x3000,80,9);
}
static int mount(SfDmarInfo*out){return sf_acpi_dmar(read_memory,0,0x1000,out);}
static uint64_t translate(uint32_t bdf,uint64_t iova){
 /* Independent walk for the fixed 3-level legacy profile, no identity fallback. */
 const uint64_t base=0x100000;uint64_t*page=tables.root;
 if(bdf>=256||!(page[0]&1))return UINT64_MAX;
 uint64_t p=page[0]&~4095ull;if(p<base||p>=base+sizeof(tables))return UINT64_MAX;
 page=(void*)((uint8_t*)&tables+p-base);if(!(page[bdf*2]&1)||((page[bdf*2+1]&7)!=1))return UINT64_MAX;p=page[bdf*2]&~4095ull;
 for(int shift=30;shift>=12;shift-=9){if(p<base||p>=base+sizeof(tables))return UINT64_MAX;page=(void*)((uint8_t*)&tables+p-base);p=page[(iova>>shift)&511];if((p&3)!=3)return UINT64_MAX;p&=~4095ull;}
 return p+(iova&4095);
}
int main(void){
 SfDmarInfo out;fixture();memcpy(saved,memory,sizeof(memory));CHECK(!mount(&out)&&out.base==SF_VTD_MMIO&&out.covered[40]&&!out.covered[41]);
 CHECK(sf_acpi_dmar(read_memory,0,0,&out)==SF_NOT_FOUND);
 for(unsigned offset=0;offset<80;offset++){memcpy(memory,saved,sizeof(memory));memory[0x3000+offset]^=1;CHECK(mount(&out)<0);}
 memcpy(memory,saved,sizeof(memory));memory[0x3034]=1;seal(0x3000,80,9);CHECK(!mount(&out)&&out.covered[255]);
 memcpy(memory,saved,sizeof(memory));memory[0x3034]=2;seal(0x3000,80,9);CHECK(mount(&out)==SF_DENIED);
 memcpy(memory,saved,sizeof(memory));put16(0x3036,1);seal(0x3000,80,9);CHECK(mount(&out)==SF_DENIED);
 memcpy(memory,saved,sizeof(memory));put16(0x3030,1);seal(0x3000,80,9);CHECK(mount(&out)==SF_DENIED);
 memcpy(memory,saved,sizeof(memory));put16(0x3032,0);seal(0x3000,80,9);CHECK(mount(&out)==SF_BAD_FORMAT);
 memcpy(memory,saved,sizeof(memory));memory[0x3049]=6;seal(0x3000,80,9);CHECK(mount(&out)==SF_BAD_FORMAT);
 memcpy(memory,saved,sizeof(memory));memory[0x304d]=1;seal(0x3000,80,9);CHECK(mount(&out)==SF_DENIED);
 memcpy(memory,saved,sizeof(memory));put64(0x2024,UINT64_MAX);seal(0x2000,44,9);CHECK(mount(&out)==SF_BAD_FORMAT);
 memcpy(memory,saved,sizeof(memory));put32(0x2004,52);put64(0x202c,0x3000);seal(0x2000,52,9);CHECK(mount(&out)==SF_BAD_FORMAT);
 memcpy(memory,saved,sizeof(memory));put32(0x3004,4097);CHECK(mount(&out)==SF_BAD_FORMAT);
 memcpy(memory,saved,sizeof(memory));memory[0x100f]=1;put32(0x1010,0x2000);seal(0x1000,20,8);memcpy(memory+0x2000,"RSDT",4);put32(0x2004,40);put32(0x2024,0x3000);seal(0x2000,40,9);CHECK(!mount(&out));
 CHECK(!sf_vtd_tables(&tables,0x100000,40,0x3000000));
 for(unsigned i=0;i<SF_DMA_BYTES;i+=512)CHECK(translate(40,SF_DMA_IOVA+i)==0x3000000+i);
 CHECK(translate(40,SF_DMA_IOVA-1)==UINT64_MAX&&translate(40,SF_DMA_IOVA+SF_DMA_BYTES)==UINT64_MAX);
 CHECK(translate(41,SF_DMA_IOVA)==UINT64_MAX&&translate(296,SF_DMA_IOVA)==UINT64_MAX);
 CHECK(translate(40,0x3000000)==UINT64_MAX&&translate(40,0x2000000)==UINT64_MAX);
 tables.context[80]=0;CHECK(translate(40,SF_DMA_IOVA)==UINT64_MAX);
 CHECK(sf_vtd_tables(&tables,0x100001,40,0x3000000)==SF_INVALID);CHECK(sf_vtd_tables(&tables,0x100000,256,0x3000000)==SF_INVALID);CHECK(sf_vtd_tables(&tables,0x100000,40,0x40000000)==SF_INVALID);
 printf("IOMMU HOST PASS: %d checks (ACPI/DMAR bounds, scopes, checksums, DMA domain boundaries)\n",checks);return 0;
}
