/* UEFI stub: embedded ELF Core 0 + authenticated bootstrap sfmod + bundle.
 * No firmware calls or allocations are made between GetMemoryMap and Exit.
 */
#include "efi_boot.h"
#include "package.h"
#include "trust.h"
extern const uint8_t __kernel_elf_start[],__kernel_elf_end[],__bundle_start[],__bundle_end[];
extern void __attribute__((ms_abi,noreturn)) efi_handoff(void*,BootInfo*,uint64_t);
static uint64_t p4[512] __attribute__((aligned(4096))),p3[512] __attribute__((aligned(4096))),p2[4][512] __attribute__((aligned(4096)));
static EFI_BOOT_SERVICES *bs;
static inline void outb(uint16_t p,uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static void log_text(const char*s){while(*s){uint8_t ready;do{__asm__ volatile("inb %1,%0":"=a"(ready):"Nd"((uint16_t)0x3fd));}while(!(ready&32));outb(0x3f8,*s++);}}
static void log_hex(uint64_t x){char b[19]="0x0000000000000000";for(int i=0;i<16;i++)b[17-i]="0123456789abcdef"[(x>>(i*4))&15];log_text(b);}
static int bounded(uint64_t off,uint64_t n,uint64_t max){return off<=max&&n<=max-off;}
static EFI_STATUS allocate(uint64_t bytes,uint64_t*addr){*addr=0x3fffffffull;return bs->AllocatePages(1,EfiLoaderData,(bytes+4095)/4096,addr);}
static EFI_STATUS core_load(void**entry){
 const uint8_t*d=__kernel_elf_start;uint64_t size=__kernel_elf_end-d;
 if(size<64||memcmp(d,"\177ELF\002\001\001",7)||sf_u16(d+16)!=2||sf_u16(d+18)!=62||sf_u16(d+54)!=56)return EFI_LOAD_ERROR;
 uint64_t table=sf_u64(d+32);unsigned count=sf_u16(d+56);if(!count||count>16||!bounded(table,count*56ull,size))return EFI_LOAD_ERROR;
 uint64_t start=sf_u64(d+24);int executable=0;
 for(unsigned i=0;i<count;i++){const uint8_t*s=d+table+i*56;if(sf_u32(s)!=1)continue;uint64_t off=sf_u64(s+8),v=sf_u64(s+16),physical=sf_u64(s+24),file=sf_u64(s+32),mem=sf_u64(s+40);
  if(!mem||file>mem||!bounded(off,file,size)||physical<0x100000||physical>=0x4000000||mem>0x4000000-physical||(physical&4095)||v!=physical||(sf_u32(s+4)&3)==3)return EFI_LOAD_ERROR;
  uint64_t end=(physical+mem+4095)&~4095ull;
  for(unsigned j=0;j<i;j++){const uint8_t*t=d+table+j*56;if(sf_u32(t)==1){uint64_t a=sf_u64(t+24),b=(a+sf_u64(t+40)+4095)&~4095ull;if(physical<b&&a<end)return EFI_LOAD_ERROR;}}
  if((sf_u32(s+4)&1)&&start>=v&&start-v<file)executable=1;
 }
 if(!executable)return EFI_LOAD_ERROR;
 for(unsigned i=0;i<count;i++){const uint8_t*s=d+table+i*56;if(sf_u32(s)!=1)continue;uint64_t physical=sf_u64(s+24),file=sf_u64(s+32),mem=sf_u64(s+40),address=physical;
  EFI_STATUS status=bs->AllocatePages(AllocateAddress,EfiLoaderData,(mem+4095)/4096,&address);if(status){log_text("[EFISTUB] AllocateAddress failed at ");log_hex(physical);log_text(" status ");log_hex(status);log_text("\n");return status;}
  memset((void*)physical,0,(mem+4095)&~4095ull);memcpy((void*)physical,d+sf_u64(s+8),file);
 }*entry=(void*)start;return EFI_SUCCESS;
}
static EFI_STATUS bootstrap_load(BootInfo*b){
 const uint8_t*data;uint64_t n;uint64_t bundle_bytes=__bundle_end-__bundle_start;
 if(sf_bundle_find(__bundle_start,bundle_bytes,"sf-init.sfmod",&data,&n))return EFI_LOAD_ERROR;
 SfPackage p;if(sf_package_open_set(&p,data,n,sf_trust_keys,sf_trust_count)||p.kind!=1)return EFI_LOAD_ERROR;
 char id[96];if(sf_manifest_text(&p,"module_id",id,sizeof(id))||strcmp(id,"org.spiritfox.init"))return EFI_LOAD_ERROR;
 b->init_entry=p.entry;b->bundle=(uint64_t)__bundle_start;b->bundle_bytes=bundle_bytes;
 for(unsigned i=0;i<p.count;i++){SfSection*s=&p.sections[i];if(s->type>3)continue;if(b->init_segment_count==8)return EFI_LOAD_ERROR;uint64_t addr;EFI_STATUS status=allocate(s->memory,&addr);if(status)return status;
  memset((void*)addr,0,(s->memory+4095)&~4095ull);memcpy((void*)addr,p.data+s->offset,s->bytes);
  b->init_segments[b->init_segment_count++]=(SfBootSegment){addr,s->rva,s->bytes,s->memory,s->flags,0};
 }log_text("[EFISTUB] verified Ed25519 bootstrap sf-init.sfmod\n");return EFI_SUCCESS;
}
EFI_STATUS __attribute__((ms_abi)) efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE*st){
 bs=st->BootServices;outb(0x3f9,0);outb(0x3fb,128);outb(0x3f8,1);outb(0x3f9,0);outb(0x3fb,3);outb(0x3fa,0xc7);outb(0x3fc,0x0b);
 log_text("[EFISTUB] SpiritFoxOS UEFI\n");
 BootInfo*b;EFI_MEMORY_DESCRIPTOR*raw;MemoryMapEntry*map;
 if(bs->AllocatePool(EfiLoaderData,sizeof(*b),(void**)&b)||bs->AllocatePool(EfiLoaderData,131072,(void**)&raw)||bs->AllocatePool(EfiLoaderData,2048*sizeof(*map),(void**)&map))return EFI_OUT_OF_RESOURCES;
 memset(b,0,sizeof(*b));b->magic=BOOT_INFO_MAGIC;b->boot_type=1;b->version=1;b->size=sizeof(*b);
 EFI_GUID acpi2=ACPI_20_TABLE_GUID,acpi1=ACPI_TABLE_GUID;
 if(st->NumberOfTableEntries>256)return EFI_LOAD_ERROR;
 for(UINTN i=0;i<st->NumberOfTableEntries;i++){EFI_CONFIGURATION_TABLE*t=&st->ConfigurationTable[i];if(!memcmp(&t->VendorGuid,&acpi2,sizeof(acpi2)))b->acpi_rsdp=(uint64_t)t->VendorTable;else if(!b->acpi_rsdp&&!memcmp(&t->VendorGuid,&acpi1,sizeof(acpi1)))b->acpi_rsdp=(uint64_t)t->VendorTable;}
 EFI_GUID guid=EFI_GOP_GUID;EFI_GRAPHICS_OUTPUT_PROTOCOL*gop=0;
 if(!bs->LocateProtocol(&guid,0,(void**)&gop)&&gop&&gop->Mode&&gop->Mode->Info){b->framebuffer_base=gop->Mode->FrameBufferBase;b->framebuffer_size=gop->Mode->FrameBufferSize;b->width=gop->Mode->Info->HorizontalResolution;b->height=gop->Mode->Info->VerticalResolution;b->pitch=gop->Mode->Info->PixelsPerScanLine*4;b->bpp=32;}
 void*entry;EFI_STATUS status=core_load(&entry);if(status){log_text("[EFISTUB] rejected Core 0 ELF\n");return status;}
 status=bootstrap_load(b);if(status){log_text("[EFISTUB] rejected bootstrap package\n");return status;}
 for(unsigned i=0;i<512;i++){p4[i]=0;p3[i]=0;}p4[0]=(uint64_t)p3|3;
 for(unsigned g=0;g<4;g++){p3[g]=(uint64_t)p2[g]|3;for(unsigned i=0;i<512;i++)p2[g][i]=g*0x40000000ull+i*0x200000ull|0x83;}
 UINTN size,key,desc;UINT32 ver;unsigned count=0;
 for(unsigned retry=0;retry<4;retry++){size=131072;status=bs->GetMemoryMap(&size,raw,&key,&desc,&ver);if(status||desc<sizeof(*raw)||size%desc||size/desc>2048)return EFI_LOAD_ERROR;count=size/desc;
  status=bs->ExitBootServices(image,key);if(!status)break;if(status!=EFI_INVALID_PARAMETER)return status;}
 if(status)return status;
 for(unsigned i=0;i<count;i++){EFI_MEMORY_DESCRIPTOR*d=(void*)((uint8_t*)raw+i*desc);map[i]=(MemoryMapEntry){d->PhysicalStart,d->VirtualStart,d->NumberOfPages,d->Attribute,d->Type,0};if(d->Type==EfiConventionalMemory)b->total_memory+=d->NumberOfPages*4096;}
 b->memory_map=(uint64_t)map;b->memory_map_size=count*sizeof(*map);b->memory_map_descriptor_size=sizeof(*map);b->memory_map_entry_count=count;
 log_text("[EFISTUB] ExitBootServices OK\n");efi_handoff(entry,b,(uint64_t)p4);
}
