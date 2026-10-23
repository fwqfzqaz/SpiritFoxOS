/* Legacy/transitional virtio PCI, queue 0, polling, one bounded request at a time.
 * IN / OUT / FLUSH, with an explicit trusted-driver write grant from Core 0.
 */
#include "libsf.h"
#include "virtio.h"
static const SfBootArgs*boot;
static uint16_t queue_size,available_index,used_index;
static uint32_t used_offset;
static uint64_t capacity,last_sector=UINT64_MAX;
static int online,writable;
static uint64_t write_first,write_end,pending_sector=UINT64_MAX;
static uint8_t cache[512],pending[512];
static uint16_t notify_offset;
static long mmio(unsigned offset,unsigned width,int write,uint32_t value){
 if(offset>16384-width||(width!=1&&width!=2&&width!=4))return SF_INVALID;volatile uint8_t*p=(void*)((uint64_t)boot->device_io+offset);
 if(write){if(width==1)*p=value;else if(width==2)*(volatile uint16_t*)p=value;else*(volatile uint32_t*)p=value;return 0;}
 return width==1?*p:width==2?*(volatile uint16_t*)p:*(volatile uint32_t*)p;
}
static long io(unsigned offset,unsigned width,int write,uint32_t value){
 if(boot->device_rights&SF_DEVICE_MODERN){unsigned actual;if(offset==18)actual=20;else if(offset==16)actual=0x3000+4u*notify_offset;else if(offset==20)actual=0x2000;else if(offset==24)actual=0x2004;else return SF_DENIED;return mmio(actual,width,write,value);}
 return sf_sys(SYS_PORT_IO,boot->device_io+offset,width|(write?256:0),value);
}
static void barrier(void){__atomic_thread_fence(__ATOMIC_SEQ_CST);}
static int stop(void){online=0;last_sector=UINT64_MAX;return sf_sys(SYS_DEVICE_RELEASE,0,0,0);}
static void address(unsigned off,uint64_t value){mmio(off,4,1,value);mmio(off+4,4,1,value>>32);}
static int start_modern(void){
 if(!(boot->device_rights&SF_DEVICE_ISOLATED)||boot->device_io!=SF_DEVICE_MMIO)return SF_DENIED;
 mmio(20,1,1,0);if(mmio(20,1,0,0))return SF_DEVICE_GONE;mmio(20,1,1,3);
 mmio(0,4,1,0);uint32_t low=mmio(4,4,0,0);mmio(0,4,1,1);uint32_t high=mmio(4,4,0,0);if((high&3)!=3)return SF_DENIED;
 writable=(boot->device_rights&SF_DEVICE_WRITE)!=0;if(writable){if((low&(1u<<5))||!(low&(1u<<9)))return SF_READ_ONLY;}else if(!(low&(1u<<5)))return SF_READ_ONLY;
 mmio(8,4,1,0);mmio(12,4,1,writable?1u<<9:1u<<5);mmio(8,4,1,1);mmio(12,4,1,3);mmio(20,1,1,11);if(mmio(20,1,0,0)!=11)return SF_DENIED;
 mmio(16,2,1,65535);mmio(22,2,1,0);long size=mmio(24,2,0,0);if(sf_virtq_layout(size,&used_offset)||mmio(28,2,0,0))return SF_BAD_FORMAT;queue_size=size;mmio(26,2,1,65535);
 notify_offset=mmio(30,2,0,0);if(notify_offset>1023)return SF_BAD_FORMAT;
 memset((void*)SF_DMA,0,SF_DMA_BYTES);*(uint16_t*)(SF_DMA+16u*queue_size)=1;
 address(32,boot->dma_physical);address(40,boot->dma_physical+16u*queue_size);address(48,boot->dma_physical+used_offset);barrier();mmio(28,2,1,1);
 int stable=0;for(unsigned i=0;i<8;i++){unsigned gen=mmio(21,1,0,0);uint32_t lo=mmio(0x2000,4,0,0),hi=mmio(0x2004,4,0,0);if(gen==mmio(21,1,0,0)){capacity=((uint64_t)hi<<32)|lo;stable=1;break;}}
 if(!stable||!capacity||capacity>UINT64_MAX/512)return SF_BAD_FORMAT;mmio(20,1,1,15);if(mmio(20,1,0,0)!=15)return SF_DEVICE_GONE;online=1;sf_log("[VIRTIO] modern ACCESS_PLATFORM transport\n");return 0;
}
static int start(void){
 if(!boot->device_io||boot->dma_virtual!=SF_DMA||!boot->dma_physical||(boot->dma_physical&4095))return SF_DENIED;
 if(boot->device_rights&SF_DEVICE_MODERN)return start_modern();
 if(io(18,1,1,0)||io(18,1,0,0)!=0)return SF_DEVICE_GONE;
 if(io(18,1,1,1)||io(18,1,1,3))return SF_DEVICE_GONE;
 long features=io(0,4,0,0);if(features<0)return features;
 writable=(boot->device_rights&SF_DEVICE_WRITE)!=0;
 if(writable){if((features&(1u<<5))||!(features&(1u<<9)))return SF_READ_ONLY;}
 else if(!(features&(1u<<5)))return SF_READ_ONLY;
 if(io(4,4,1,writable?(1u<<9):(1u<<5))||io(14,2,1,0))return SF_DEVICE_GONE;
 long size=io(12,2,0,0);if(size<0||sf_virtq_layout(size,&used_offset))return SF_BAD_FORMAT;queue_size=size;
 if(io(8,4,0,0)!=0)return SF_BUSY;
 memset((void*)SF_DMA,0,SF_DMA_BYTES);*(uint16_t*)(SF_DMA+16u*queue_size)=1; /* NO_INTERRUPT */
 if(io(8,4,1,boot->dma_physical>>12))return SF_DEVICE_GONE;
 uint32_t hi=0,lo=0;int stable=0;for(unsigned i=0;i<8;i++){long a=io(24,4,0,0),b=io(20,4,0,0),c=io(24,4,0,0);if(a<0||b<0||c<0)return SF_DEVICE_GONE;if(a==c){hi=a;lo=b;stable=1;break;}}
 capacity=((uint64_t)hi<<32)|lo;if(!stable||!capacity||capacity>UINT64_MAX/512)return SF_BAD_FORMAT;
 barrier();if(io(18,1,1,7)||io(18,1,0,0)!=7)return SF_DEVICE_GONE;online=1;return 0;
}
static int request_at(unsigned type,uint64_t sector,uint64_t target,int header_probe){
 if(!online)return SF_DEVICE_GONE;if(sector>=capacity)return SF_INVALID;
 SfVirtqDescriptor*d=(void*)SF_DMA;SfVirtioRequest*r=(void*)(SF_DMA+16384);volatile uint8_t*status=(void*)(SF_DMA+16400);uint8_t*data=(void*)(SF_DMA+20480);
 *r=(SfVirtioRequest){type,0,sector};*status=255;if(type==0)memset(data,0,512);else if(type==1)memcpy(data,pending,512);
 d[0]=(SfVirtqDescriptor){boot->dma_physical+16384,16,1,type==4?2:1};d[1]=(SfVirtqDescriptor){boot->dma_physical+20480,512,type==0?3:1,2};d[2]=(SfVirtqDescriptor){boot->dma_physical+16400,1,2,0};
 if(target){if(header_probe)d[0].address=target;else d[1].address=target;}
 volatile uint16_t*avail=(void*)(SF_DMA+16u*queue_size);volatile uint16_t*used=(void*)(SF_DMA+used_offset);
 avail[2+(available_index%queue_size)]=0;barrier();available_index++;avail[1]=available_index;barrier();
 if(io(16,2,1,0)){stop();return SF_DEVICE_GONE;}
 uint64_t deadline=sf_ticks()+100;while(used[1]==used_index){if(sf_ticks()>=deadline){stop();return SF_TIMEOUT;}sf_yield();}
 barrier();uint16_t completed=used[1];volatile uint32_t*entry=(void*)(SF_DMA+used_offset+4+8u*(used_index%queue_size));
 if((uint16_t)(completed-used_index)!=1||entry[0]!=0){stop();return SF_BAD_FORMAT;}used_index=completed;
 if(*status!=0){last_sector=UINT64_MAX;return SF_DEVICE_GONE;}if(entry[1]!=(type==0?513u:1u)){stop();return SF_BAD_FORMAT;}
 if(type==0){memcpy(cache,data,512);last_sector=sector;}else last_sector=UINT64_MAX;return 0;
}
static int request(unsigned type,uint64_t sector){return request_at(type,sector,0,0);}
static int read_sector(uint64_t sector){return last_sector==sector?0:request(0,sector);}
long sf_main(const SfBootArgs*a,size_t size){
 boot=a;if(size!=sizeof(*a)||a->abi_major!=1)return SF_ABI_MISMATCH;int startup=start();if(startup)stop();
 sf_log(startup?"[VIRTIO] device initialization refused\n":writable?"[VIRTIO] flush-capable write service in ring 3\n":"[VIRTIO] readonly block service in ring 3\n");
 for(;;){SfMessage m;long sender=sf_recv(a->service,&m,0);if(sender<0){stop();return sender;}int result=0;
  if(sender!=1)result=SF_DENIED;
  else if(m.h.opcode==OP_BLOCK_STOP){result=stop();m.status=result;sf_send(a->manager,&m);continue;}
  else if(startup)result=startup;
  else if(!online)result=SF_DEVICE_GONE;
  else if(m.h.opcode==OP_BLOCK_INFO){SfBlockInfo info={capacity,512,!writable};memcpy(m.data,&info,sizeof(info));m.length=sizeof(info);}
  else if(m.h.opcode==OP_BLOCK_DMA_PROBE){if(!(boot->device_rights&SF_DEVICE_ISOLATED)||!m.offset||(m.offset&4095)||m.mode>1)result=SF_DENIED;else{sf_log("[VIRTIO] deliberate out-of-domain DMA probe\n");result=request_at(0,0,m.offset,m.mode);}}
  else if(m.h.opcode==OP_BLOCK_WRITE_RANGE){if(!writable)result=SF_READ_ONLY;else if((write_end&&(write_first!=m.offset||write_end!=m.handle))||m.offset>=m.handle||m.handle>capacity)result=SF_INVALID;else{write_first=m.offset;write_end=m.handle;}}
  else if(m.h.opcode==OP_BLOCK_FLUSH){pending_sector=UINT64_MAX;result=writable&&write_end?request(4,0):SF_READ_ONLY;}
  else if(m.h.opcode==OP_BLOCK_WRITE){
   if(!writable||!write_end)result=SF_READ_ONLY;
   else if(!sf_block_range(capacity,m.offset,m.mode,m.length)||m.offset/512<write_first||m.offset/512>=write_end){pending_sector=UINT64_MAX;result=SF_INVALID;}
   else if(!m.mode){memcpy(pending,m.data,256);pending_sector=m.offset/512;}
   else if(pending_sector!=m.offset/512){pending_sector=UINT64_MAX;result=SF_INVALID;}
   else{memcpy(pending+256,m.data,256);pending_sector=UINT64_MAX;result=request(1,m.offset/512);}
  }
  else if(m.h.opcode==OP_BLOCK_READ){if(!sf_block_range(capacity,m.offset,m.mode,m.length))result=SF_INVALID;else{result=read_sector(m.offset/512);if(!result)memcpy(m.data,cache+m.mode,256);}}
  else result=SF_INVALID;
  m.status=result;if(result)m.length=0;sf_send(a->manager,&m);
 }
}
