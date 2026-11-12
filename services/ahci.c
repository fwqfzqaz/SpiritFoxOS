/* AHCI/SATA 块驱动：真实硬件上的 01/06/01 存储控制器，轮询命令完成。
 * 与 virtio-blk 共用 OP_BLOCK_* 协议，sf-init 无需区分底层控制器。
 * DMA 区布局：命令列表 0x0000、FIS 0x0400、命令表 0x0800、IDENTIFY 0x1000、
 * 数据窗口 0x2000，全部位于内核授予的 64 KiB DMA 区。 */
#include "libsf.h"
#include "virtio.h"

#define FIS_OFF 0x0400u
#define TABLE_OFF 0x0800u
#define IDENT_OFF 0x1000u
#define DATA_OFF 0x2000u

static const SfBootArgs*boot;
static uint32_t port;
static uint64_t capacity,last_sector=UINT64_MAX;
static int online,writable;
static uint64_t write_first,write_end,pending_sector=UINT64_MAX;
static uint8_t cache[512],pending[512];

static void barrier(void){__atomic_thread_fence(__ATOMIC_SEQ_CST);}
static uint32_t reg(uint32_t off){return *(volatile uint32_t*)((uint64_t)boot->device_io+off);}
static void regw(uint32_t off,uint32_t value){*(volatile uint32_t*)((uint64_t)boot->device_io+off)=value;}
static uint32_t pr(uint32_t off){return *(volatile uint32_t*)((uint64_t)boot->device_io+0x100u+port*0x80u+off);}
static void prw(uint32_t off,uint32_t value){*(volatile uint32_t*)((uint64_t)boot->device_io+0x100u+port*0x80u+off)=value;}

static int command(uint8_t op,uint64_t lba,uint16_t count,uint64_t bus_data,int write,uint32_t bytes){
 uint8_t*fis=(void*)(SF_DMA+FIS_OFF),*table=(void*)(SF_DMA+TABLE_OFF),*list=(void*)(SF_DMA);
 memset(fis,0,64);fis[0]=0x27;fis[1]=0x80;fis[2]=op;fis[3]=0;
 fis[4]=(uint8_t)lba;fis[5]=(uint8_t)(lba>>8);fis[6]=(uint8_t)(lba>>16);fis[7]=0x40;
 fis[8]=(uint8_t)(lba>>24);fis[9]=(uint8_t)(lba>>32);fis[10]=(uint8_t)(lba>>40);fis[11]=0;
 fis[12]=(uint8_t)count;fis[13]=(uint8_t)(count>>8);fis[14]=0;fis[15]=0;
 memset(table,0,0x100);memcpy(table,fis,16);
 if(bytes){*(uint64_t*)(table+0x80)=bus_data;*(uint32_t*)(table+0x8c)=bytes-1;} /* PRDT DBC 在 +0x0C */
 memset(list,0,0x400);
 *(uint16_t*)list=(uint16_t)(5u|(write?1u<<6:0));*(uint16_t*)(list+2)=bytes?1:0;
 uint64_t ctba=boot->dma_physical+TABLE_OFF;*(uint32_t*)(list+8)=(uint32_t)ctba;*(uint32_t*)(list+12)=(uint32_t)(ctba>>32);
 barrier();prw(0x10,0xffffffffu);prw(0x38,1u);
 uint64_t deadline=sf_ticks()+1000;
 while(pr(0x38)&1u){if(sf_ticks()>=deadline){sf_log("[AHCI] command timeout\n");return SF_TIMEOUT;}sf_yield();}
 uint32_t is=pr(0x10);prw(0x10,is);
 if(is&((1u<<30)|(1u<<27)|(1u<<5)|(1u<<4)|(1u<<3)|(1u<<2))){sf_log("[AHCI] command error is=");sf_number(is);sf_log(" tfd=");sf_number(pr(0x20));sf_log("\n");return SF_DEVICE_GONE;}
 barrier();return 0;
}

static int stop(void){online=0;last_sector=UINT64_MAX;return sf_sys(SYS_DEVICE_RELEASE,0,0,0);}

static int start(void){
 if(!boot->device_io||boot->device_io!=SF_DEVICE_MMIO||boot->dma_virtual!=SF_DMA||!boot->dma_physical||(boot->dma_physical&4095))return SF_DENIED;
 /* GHC.AE: 固件未启用 AHCI 模式时端口引擎不会启动。 */
 uint32_t ghc=reg(4);if(!(ghc&(1u<<31)))regw(4,ghc|(1u<<31));
 uint32_t pi=reg(0x0c);port=32;
 for(unsigned p=0;p<32;p++)if(pi&(1u<<p)){
  uint32_t ssts=*(volatile uint32_t*)((uint64_t)boot->device_io+0x100u+p*0x80u+0x28);
  uint32_t sig=*(volatile uint32_t*)((uint64_t)boot->device_io+0x100u+p*0x80u+0x24);
  if((ssts&0xf)==3&&((ssts>>8)&0xf)==1&&sig==0x101){port=p;break;}
 }
 if(port==32){sf_log("[AHCI] no SATA port\n");return SF_NOT_FOUND;}
 uint32_t cmd=pr(0x18);prw(0x18,cmd&~1u);
 uint64_t deadline=sf_ticks()+100;
 while(pr(0x18)&(1u<<15)){if(sf_ticks()>=deadline){sf_log("[AHCI] stop timeout\n");return SF_TIMEOUT;}sf_yield();}
 prw(0x18,pr(0x18)&~(1u<<4));
 deadline=sf_ticks()+100;while(pr(0x18)&(1u<<14)){if(sf_ticks()>=deadline){sf_log("[AHCI] FIS receive stop timeout\n");return SF_TIMEOUT;}sf_yield();}
 prw(0x14,0);prw(0x10,0xffffffffu);
 uint64_t clb=boot->dma_physical,fis=boot->dma_physical+FIS_OFF;
 prw(0x00,(uint32_t)clb);prw(0x04,(uint32_t)(clb>>32));prw(0x08,(uint32_t)fis);prw(0x0c,(uint32_t)(fis>>32));
 uint32_t cap=reg(0);if(!(cap&(1u<<31))&&((boot->dma_physical>>32)||(fis>>32)))return SF_DENIED;
 prw(0x18,pr(0x18)|(1u<<4));deadline=sf_ticks()+100;
 while(!(pr(0x18)&(1u<<14))){if(sf_ticks()>=deadline){sf_log("[AHCI] FRE timeout cmd=");sf_number(pr(0x18));sf_log(" ghc=");sf_number(reg(4));sf_log("\n");return SF_TIMEOUT;}sf_yield();}
 prw(0x18,pr(0x18)|1u);deadline=sf_ticks()+100;
 while(!(pr(0x18)&(1u<<15))){if(sf_ticks()>=deadline){sf_log("[AHCI] ST timeout\n");return SF_TIMEOUT;}sf_yield();}
 if(command(0xEC,0,0,boot->dma_physical+IDENT_OFF,0,512)){sf_log("[AHCI] identify failed\n");return SF_DEVICE_GONE;}
 uint16_t*id=(void*)(SF_DMA+IDENT_OFF);uint64_t sectors=0;
 if(id[83]&(1u<<10)){uint64_t max=*(uint64_t*)((uint8_t*)id+400);if(max)sectors=max+1;} /* words 200..203 = byte 400 */
 if(!sectors)sectors=((uint64_t)id[61]<<16)|id[60];
 if(!sectors||sectors>UINT64_MAX/512){sf_log("[AHCI] bad capacity lbas=");sf_number(sectors);sf_log(" w83=");sf_number(id[83]);sf_log(" w60=");sf_number(id[60]);sf_log(" w61=");sf_number(id[61]);sf_log("\n");return SF_BAD_FORMAT;}
 int media_ro=(id[87]&(1u<<5))!=0;
 writable=(boot->device_rights&SF_DEVICE_WRITE)!=0;
 if(writable&&media_ro){sf_log("[AHCI] media read-only\n");return SF_READ_ONLY;}
 if(!writable&&!media_ro)return SF_READ_ONLY; /* 让 sf-init 升级为写授权 */
 capacity=sectors;online=1;
 sf_log(writable?"[AHCI] SATA disk online, read-write\n":"[AHCI] SATA disk online, read-only\n");
 return 0;
}

static int io_sector(int write,uint64_t sector){
 if(sector>=capacity)return SF_INVALID;
 uint8_t*data=(void*)(SF_DMA+DATA_OFF);
 if(write)memcpy(data,pending,512);else memset(data,0,512);
 int r=command(write?0x35:0x25,sector,1,boot->dma_physical+DATA_OFF,write,512);
 if(!r&&!write)memcpy(cache,data,512);
 return r;
}

static int read_block(uint64_t sector){
 if(last_sector==sector)return 0;
 int r=io_sector(0,sector);if(!r)last_sector=sector;return r;
}

static int flush(void){return command(0xEA,0,0,0,0,0);}

long sf_main(const SfBootArgs*a,size_t size){
 boot=a;if(size!=sizeof(*a)||a->abi_major!=1)return SF_ABI_MISMATCH;
 int startup=start();if(startup)stop();
 for(;;){SfMessage m;long sender=sf_recv(a->service,&m,0);if(sender<0){stop();return sender;}int result=0;
  if(m.h.opcode==OP_BLOCK_STOP){result=stop();m.status=result;sf_send(a->manager,&m);continue;}
  else if(startup)result=startup;
  else if(!online)result=SF_DEVICE_GONE;
  else if(m.h.opcode==OP_BLOCK_INFO){SfBlockInfo info={capacity,512,!writable};memcpy(m.data,&info,sizeof(info));m.length=sizeof(info);}
  else if(m.h.opcode==OP_BLOCK_DMA_PROBE)result=SF_DENIED;
  else if(m.h.opcode==OP_BLOCK_WRITE_RANGE){if(!writable)result=SF_READ_ONLY;else if((write_end&&(write_first!=m.offset||write_end!=m.handle))||m.offset>=m.handle||m.handle>capacity)result=SF_INVALID;else{write_first=m.offset;write_end=m.handle;}}
  else if(m.h.opcode==OP_BLOCK_FLUSH){pending_sector=UINT64_MAX;result=writable&&write_end?flush():SF_READ_ONLY;}
  else if(m.h.opcode==OP_BLOCK_WRITE){
   if(!writable||!write_end)result=SF_READ_ONLY;
   else if(!sf_block_range(capacity,m.offset,m.mode,m.length)||m.offset/512<write_first||m.offset/512>=write_end){pending_sector=UINT64_MAX;result=SF_INVALID;}
   else if(!m.mode){memcpy(pending,m.data,256);pending_sector=m.offset/512;}
   else if(pending_sector!=m.offset/512){pending_sector=UINT64_MAX;result=SF_INVALID;}
   else{memcpy(pending+256,m.data,256);pending_sector=UINT64_MAX;result=io_sector(1,m.offset/512);if(!result)last_sector=UINT64_MAX;}
  }
  else if(m.h.opcode==OP_BLOCK_READ){if(!sf_block_range(capacity,m.offset,m.mode,m.length))result=SF_INVALID;else{result=read_block(m.offset/512);if(!result)memcpy(m.data,cache+m.mode,256);}}
  else result=SF_INVALID;
  m.status=result;if(result)m.length=0;sf_send(a->manager,&m);
 }
}
