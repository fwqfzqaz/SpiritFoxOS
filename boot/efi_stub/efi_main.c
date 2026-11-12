/* UEFI stub: embedded ELF Core 0 + authenticated bootstrap sfmod + bundle.
 * No firmware calls or allocations are made between GetMemoryMap and Exit.
 */
#include "efi_boot.h"
#include "package.h"
#include "trust.h"
#include "font8x8.h"
extern const uint8_t __kernel_elf_start[],__kernel_elf_end[],__bundle_start[],__bundle_end[];
extern const uint8_t efi_handoff_low_start[],efi_handoff_low_end[];
static EFI_BOOT_SERVICES *bs;
static inline void outb(uint16_t p,uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
/* 真机通常没有串口：同时把启动日志画到固件 GOP framebuffer，否则失败时
 * 只能看到黑屏加光标。字体为 8x8 点阵，2 倍放大成 16x16 单元格。 */
#define FB_BG 0x00101820u
#define FB_FG 0x00e8f0f8u
static volatile uint32_t *con_fb;
static uint32_t con_pitch,con_cols,con_rows,con_x,con_y;
static volatile uint32_t *fb_cell(uint32_t x,uint32_t y){return con_fb+(uint64_t)y*(con_pitch/4u)+x;}
static void fb_clear(void){
 uint64_t pixels=(uint64_t)con_rows*16u*(con_pitch/4u);
 for(uint64_t i=0;i<pixels;i++)((volatile uint32_t*)con_fb)[i]=FB_BG;
 con_x=con_y=0;
}
static void fb_scroll(void){
 uint32_t line=16u*(con_pitch/4u),total=(con_rows-1u)*line;
 for(uint32_t i=0;i<total;i++)((volatile uint32_t*)con_fb)[i]=((volatile uint32_t*)con_fb)[i+line];
 for(uint32_t i=0;i<line;i++)((volatile uint32_t*)con_fb)[total+i]=FB_BG;
 con_y=con_rows-1u;
}
static void fb_putc(char c){
 if(!con_fb)return;
 if(c=='\n'){con_x=0;if(++con_y>=con_rows)fb_scroll();return;}
 if(c=='\r'){con_x=0;return;}
 if(c<0x20||c>0x7e)return;
 if(con_x>=con_cols){con_x=0;if(++con_y>=con_rows)fb_scroll();}
 const unsigned char*g=sf_font8x8[c-0x20];
 uint32_t px=con_x*16u,py=con_y*16u;
 for(unsigned row=0;row<8;row++){
  unsigned bits=g[row];
  for(unsigned col=0;col<8;col++){
   uint32_t color=(bits&(1u<<col))?FB_FG:FB_BG;
   volatile uint32_t*p=fb_cell(px+col*2u,py+row*2u);
   p[0]=color;p[1]=color;p[con_pitch/4u]=color;p[con_pitch/4u+1]=color;
  }
 }
 con_x++;
}
static void fb_begin(uint64_t base,uint64_t bytes,uint32_t width,uint32_t height,uint32_t pitch,uint32_t bpp){
 con_fb=0;con_pitch=con_cols=con_rows=con_x=con_y=0;
 if(bpp!=32||!base||!width||!height||!pitch||(pitch&3)||width>8192||height>8192||pitch<width*4u)return;
 if(base>0x100000000ull||bytes>0x100000000ull-base||(uint64_t)height*pitch>bytes)return;
 con_fb=(volatile uint32_t*)(uint64_t)base;con_pitch=pitch;
 con_cols=width/16u;con_rows=height/16u;
 if(!con_cols||!con_rows){con_fb=0;return;}
 fb_clear();
}
static void log_text(const char*s){while(*s){uint8_t ready;do{__asm__ volatile("inb %1,%0":"=a"(ready):"Nd"((uint16_t)0x3fd));}while(!(ready&32));outb(0x3f8,*s);fb_putc(*s);s++;}}
static void log_hex(uint64_t x){char b[19]="0x0000000000000000";for(int i=0;i<16;i++)b[17-i]="0123456789abcdef"[(x>>(i*4))&15];log_text(b);}
static void log_num(uint64_t v){char b[21];unsigned n=0;do{b[n++]=(char)('0'+v%10);v/=10;}while(v);for(unsigned i=0;i<n/2;i++){char t=b[i];b[i]=b[n-1-i];b[n-1-i]=t;}b[n]=0;log_text(b);}
static int bounded(uint64_t off,uint64_t n,uint64_t max){return off<=max&&n<=max-off;}
/* 真实固件常把 AllocateAnyPages 分配到 4 GiB 以上的高位内存，而内核只信任
 * 1 GiB 以下的身份映射区域。这里统一用 AllocateMaxAddress 限制在 1 GiB 内。 */
static EFI_STATUS allocate(uint64_t bytes,uint64_t*addr){*addr=0x3ffff000ull;return bs->AllocatePages(AllocateMaxAddress,EfiLoaderData,(bytes+4095)/4096,addr);}
/* 低内存跳板必须可执行：单独申请 EfiLoaderCode。 */
static EFI_STATUS allocate_code(uint64_t bytes,uint64_t*addr){*addr=0x3ffff000ull;return bs->AllocatePages(AllocateMaxAddress,EfiLoaderCode,(bytes+4095)/4096,addr);}
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
 if(sf_bundle_find(__bundle_start,bundle_bytes,"sf-init.sfmod",&data,&n)){log_text("[EFISTUB] bundle find failed\n");return EFI_LOAD_ERROR;}
 SfPackage p;if(sf_package_open_trusted(&p,data,n,&sf_trust_store)||p.kind!=1){log_text("[EFISTUB] init package rejected\n");return EFI_LOAD_ERROR;}
 char id[96];if(sf_manifest_text(&p,"module_id",id,sizeof(id))||strcmp(id,"org.spiritfox.init")){log_text("[EFISTUB] init module id mismatch\n");return EFI_LOAD_ERROR;}
 b->init_entry=p.entry;
 /* 嵌入式 bundle 随 EFI 镜像由固件装载，真实硬件上可能落在 1 GiB 以上；
  * 内核只接受 1 GiB 以下的 boot 对象，因此显式复制到低内存。 */
 uint64_t bundle_addr;EFI_STATUS status=allocate(bundle_bytes,&bundle_addr);
 if(status){log_text("[EFISTUB] bundle alloc failed bytes ");log_hex(bundle_bytes);log_text(" status ");log_hex(status);log_text("\n");return status;}memcpy((void*)bundle_addr,__bundle_start,bundle_bytes);
 b->bundle=bundle_addr;b->bundle_bytes=bundle_bytes;
 for(unsigned i=0;i<p.count;i++){SfSection*s=&p.sections[i];if(s->type>3)continue;if(b->init_segment_count==8)return EFI_LOAD_ERROR;uint64_t addr;EFI_STATUS status=allocate(s->memory,&addr);if(status){log_text("[EFISTUB] segment alloc failed bytes ");log_hex(s->memory);log_text(" status ");log_hex(status);log_text("\n");return status;}
  memset((void*)addr,0,(s->memory+4095)&~4095ull);memcpy((void*)addr,p.data+s->offset,s->bytes);
  b->init_segments[b->init_segment_count++]=(SfBootSegment){addr,s->rva,s->bytes,s->memory,s->flags,0};
 }log_text("[EFISTUB] verified Ed25519 bootstrap sf-init.sfmod\n");return EFI_SUCCESS;
}
EFI_STATUS __attribute__((ms_abi)) efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE*st){
 bs=st->BootServices;outb(0x3f9,0);outb(0x3fb,128);outb(0x3f8,1);outb(0x3f9,0);outb(0x3fb,3);outb(0x3fa,0xc7);outb(0x3fc,0x0b);
 /* GOP 必须先于日志定位：真机常常没有串口，后续日志要同时画到屏幕。 */
 EFI_GUID guid=EFI_GOP_GUID;EFI_GRAPHICS_OUTPUT_PROTOCOL*gop=0;
 EFI_STATUS gs=bs->LocateProtocol(&guid,0,(void**)&gop);
 if(gs||!gop||!gop->Mode||!gop->Mode->Info)gop=0;
 if(gop)fb_begin(gop->Mode->FrameBufferBase,gop->Mode->FrameBufferSize,gop->Mode->Info->HorizontalResolution,gop->Mode->Info->VerticalResolution,gop->Mode->Info->PixelsPerScanLine*4,32);
 log_text("[EFISTUB] SpiritFoxOS UEFI\n");
 if(gop){log_text("[EFISTUB] fb ");log_num(gop->Mode->Info->HorizontalResolution);log_text("x");log_num(gop->Mode->Info->VerticalResolution);log_text(" @");log_hex(gop->Mode->FrameBufferBase);log_text("\n");}
 else{log_text("[EFISTUB] no GOP status=");log_hex(gs);log_text("\n");}
 uint64_t b_addr=0,raw_addr=0,map_addr=0,pt_addr=0,stack_addr=0,thunk_addr=0;
 if(allocate(sizeof(BootInfo),&b_addr)||allocate(131072,&raw_addr)||allocate(2048*sizeof(MemoryMapEntry),&map_addr)||allocate(6*4096,&pt_addr)||allocate(4096,&stack_addr)||allocate_code(4096,&thunk_addr)){log_text("[EFISTUB] low memory allocation failed\n");return EFI_OUT_OF_RESOURCES;}
 BootInfo*b=(void*)b_addr;EFI_MEMORY_DESCRIPTOR*raw=(void*)raw_addr;MemoryMapEntry*map=(void*)map_addr;
 memset(b,0,sizeof(*b));memset(raw,0,131072);memset(map,0,2048*sizeof(*map));b->magic=BOOT_INFO_MAGIC;b->boot_type=1;b->version=1;b->size=sizeof(*b);
 EFI_GUID acpi2=ACPI_20_TABLE_GUID,acpi1=ACPI_TABLE_GUID;
 if(st->NumberOfTableEntries>256)return EFI_LOAD_ERROR;
 for(UINTN i=0;i<st->NumberOfTableEntries;i++){EFI_CONFIGURATION_TABLE*t=&st->ConfigurationTable[i];if(!memcmp(&t->VendorGuid,&acpi2,sizeof(acpi2)))b->acpi_rsdp=(uint64_t)t->VendorTable;else if(!b->acpi_rsdp&&!memcmp(&t->VendorGuid,&acpi1,sizeof(acpi1)))b->acpi_rsdp=(uint64_t)t->VendorTable;}
 if(gop){b->framebuffer_base=gop->Mode->FrameBufferBase;b->framebuffer_size=gop->Mode->FrameBufferSize;b->width=gop->Mode->Info->HorizontalResolution;b->height=gop->Mode->Info->VerticalResolution;b->pitch=gop->Mode->Info->PixelsPerScanLine*4;b->bpp=32;}
 void*entry;EFI_STATUS status=core_load(&entry);if(status){log_text("[EFISTUB] rejected Core 0 ELF\n");return status;}
 log_text("[EFISTUB] Core 0 loaded\n");
 status=bootstrap_load(b);if(status){log_text("[EFISTUB] rejected bootstrap package\n");return status;}
 /* 页表与交接跳板必须位于 1 GiB 以下的低内存：真实固件可能把 EFI 镜像
  * 装到 4 GiB 以上，切换 CR3 后原镜像地址不再可见，必须从低内存跳板
  * 完成 CR3/栈切换，否则会三重故障（黑屏死机）。 */
 uint64_t*p4=(void*)pt_addr,*p3=(void*)(pt_addr+4096);uint64_t(*p2)[512]=(void*)(pt_addr+8192);
 for(unsigned i=0;i<512;i++){p4[i]=0;p3[i]=0;}
 p4[0]=(pt_addr+4096)|3;
 for(unsigned g=0;g<4;g++){p3[g]=((pt_addr+8192)+g*4096ull)|3;for(unsigned i=0;i<512;i++)p2[g][i]=g*0x40000000ull+i*0x200000ull|0x83;}
 memcpy((void*)thunk_addr,efi_handoff_low_start,(uint64_t)(efi_handoff_low_end-efi_handoff_low_start));
 UINTN size,key,desc;UINT32 ver;unsigned count=0;
 for(unsigned retry=0;retry<4;retry++){size=131072;status=bs->GetMemoryMap(&size,raw,&key,&desc,&ver);if(status||desc<sizeof(*raw)||size%desc||size/desc>2048)return EFI_LOAD_ERROR;count=size/desc;
  status=bs->ExitBootServices(image,key);if(!status)break;if(status!=EFI_INVALID_PARAMETER)return status;}
 if(status){log_text("[EFISTUB] ExitBootServices failed\n");return status;}
 for(unsigned i=0;i<count;i++){EFI_MEMORY_DESCRIPTOR*d=(void*)((uint8_t*)raw+i*desc);map[i]=(MemoryMapEntry){d->PhysicalStart,d->VirtualStart,d->NumberOfPages,d->Attribute,d->Type,0};if(d->Type==EfiConventionalMemory)b->total_memory+=d->NumberOfPages*4096;}
 b->memory_map=(uint64_t)map;b->memory_map_size=count*sizeof(*map);b->memory_map_descriptor_size=sizeof(*map);b->memory_map_entry_count=count;
 log_text("[EFISTUB] ExitBootServices OK\n");
 ((void(__attribute__((ms_abi))*)(void*,BootInfo*,uint64_t,uint64_t))(uint64_t)thunk_addr)(entry,b,pt_addr,stack_addr+4096);
 return EFI_SUCCESS;
}
