/* Legacy/transitional virtio PCI, queue 0, polling, one bounded request at a time.
 * Only IN requests are issued. Hardware authorization comes from Core 0.
 */
#include "libsf.h"
#include "virtio.h"
static const SfBootArgs*boot;
static uint16_t queue_size,available_index,used_index;
static uint32_t used_offset;
static uint64_t capacity,last_sector=UINT64_MAX;
static int online;
static uint8_t cache[512];
static long io(unsigned offset,unsigned width,int write,uint32_t value){return sf_sys(SYS_PORT_IO,boot->device_io+offset,width|(write?256:0),value);}
static void barrier(void){__atomic_thread_fence(__ATOMIC_SEQ_CST);}
static int stop(void){online=0;last_sector=UINT64_MAX;return sf_sys(SYS_DEVICE_RELEASE,0,0,0);}
static int start(void){
 if(!boot->device_io||boot->dma_virtual!=SF_DMA||!boot->dma_physical||(boot->dma_physical&4095))return SF_DENIED;
 if(io(18,1,1,0)||io(18,1,0,0)!=0)return SF_DEVICE_GONE;
 if(io(18,1,1,1)||io(18,1,1,3))return SF_DEVICE_GONE;
 long features=io(0,4,0,0);if(features<0)return features;
 /* Refuse writable media until the separate persistence milestone exists. */
 if(!(features&(1u<<5)))return SF_READ_ONLY;
 if(io(4,4,1,1u<<5)||io(14,2,1,0))return SF_DEVICE_GONE;
 long size=io(12,2,0,0);if(size<0||sf_virtq_layout(size,&used_offset))return SF_BAD_FORMAT;queue_size=size;
 if(io(8,4,0,0)!=0)return SF_BUSY;
 memset((void*)SF_DMA,0,SF_DMA_BYTES);*(uint16_t*)(SF_DMA+16u*queue_size)=1; /* NO_INTERRUPT */
 if(io(8,4,1,boot->dma_physical>>12))return SF_DEVICE_GONE;
 uint32_t hi=0,lo=0;int stable=0;for(unsigned i=0;i<8;i++){long a=io(24,4,0,0),b=io(20,4,0,0),c=io(24,4,0,0);if(a<0||b<0||c<0)return SF_DEVICE_GONE;if(a==c){hi=a;lo=b;stable=1;break;}}
 capacity=((uint64_t)hi<<32)|lo;if(!stable||!capacity||capacity>UINT64_MAX/512)return SF_BAD_FORMAT;
 barrier();if(io(18,1,1,7)||io(18,1,0,0)!=7)return SF_DEVICE_GONE;online=1;return 0;
}
static int read_sector(uint64_t sector){
 if(!online)return SF_DEVICE_GONE;if(sector>=capacity)return SF_INVALID;if(last_sector==sector)return 0;
 SfVirtqDescriptor*d=(void*)SF_DMA;SfVirtioRequest*r=(void*)(SF_DMA+16384);volatile uint8_t*status=(void*)(SF_DMA+16400);uint8_t*data=(void*)(SF_DMA+20480);
 *r=(SfVirtioRequest){0,0,sector};*status=255;memset(data,0,512);
 d[0]=(SfVirtqDescriptor){boot->dma_physical+16384,16,1,1};d[1]=(SfVirtqDescriptor){boot->dma_physical+20480,512,3,2};d[2]=(SfVirtqDescriptor){boot->dma_physical+16400,1,2,0};
 volatile uint16_t*avail=(void*)(SF_DMA+16u*queue_size);volatile uint16_t*used=(void*)(SF_DMA+used_offset);
 avail[2+(available_index%queue_size)]=0;barrier();available_index++;avail[1]=available_index;barrier();
 if(io(16,2,1,0)){stop();return SF_DEVICE_GONE;}
 uint64_t deadline=sf_ticks()+100;while(used[1]==used_index){if(sf_ticks()>=deadline){stop();return SF_TIMEOUT;}sf_yield();}
 barrier();uint16_t completed=used[1];volatile uint32_t*entry=(void*)(SF_DMA+used_offset+4+8u*(used_index%queue_size));
 if((uint16_t)(completed-used_index)!=1||entry[0]!=0){stop();return SF_BAD_FORMAT;}used_index=completed;
 if(*status!=0){last_sector=UINT64_MAX;return SF_DEVICE_GONE;}if(entry[1]!=513){stop();return SF_BAD_FORMAT;}
 memcpy(cache,data,512);last_sector=sector;return 0;
}
long sf_main(const SfBootArgs*a,size_t size){
 boot=a;if(size!=sizeof(*a)||a->abi_major!=1)return SF_ABI_MISMATCH;int startup=start();if(startup)stop();
 sf_log(startup?"[VIRTIO] device initialization refused\n":"[VIRTIO] readonly block service in ring 3\n");
 for(;;){SfMessage m;long sender=sf_recv(a->service,&m,0);if(sender<0){stop();return sender;}int result=0;
  if(sender!=1)result=SF_DENIED;
  else if(m.h.opcode==OP_BLOCK_STOP){result=stop();m.status=result;sf_send(a->manager,&m);continue;}
  else if(startup)result=startup;
  else if(!online)result=SF_DEVICE_GONE;
  else if(m.h.opcode==OP_BLOCK_INFO){SfBlockInfo info={capacity,512,1};memcpy(m.data,&info,sizeof(info));m.length=sizeof(info);}
  else if(m.h.opcode==OP_BLOCK_WRITE)result=SF_READ_ONLY;
  else if(m.h.opcode==OP_BLOCK_READ){if(!sf_block_range(capacity,m.offset,m.mode,m.length))result=SF_INVALID;else{result=read_sector(m.offset/512);if(!result)memcpy(m.data,cache+m.mode,256);}}
  else result=SF_INVALID;
  m.status=result;if(result)m.length=0;sf_send(a->manager,&m);
 }
}
