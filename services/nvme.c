/* NVMe 块驱动：真实硬件上的 01/08/02 控制器，轮询完成队列（无需 MSI）。
 * 与 virtio-blk 共用 OP_BLOCK_* 协议。DMA 区布局：
 * 管理 SQ 0x0000 / 管理 CQ 0x1000 / IO SQ 0x2000 / IO CQ 0x3000 /
 * IDENTIFY 0x4000 / 数据窗口 0x5000。 */
#include "libsf.h"
#include "virtio.h"

#define ADMIN_SQ 0x0000u
#define ADMIN_CQ 0x1000u
#define IO_SQ 0x2000u
#define IO_CQ 0x3000u
#define IDENT_OFF 0x4000u
#define DATA_OFF 0x5000u
#define QDEPTH 4u
#define DOORBELL 0x1000u

static const SfBootArgs*boot;
static uint32_t stride,dstrd;
static uint32_t lba_bytes,ratio,total_halves;
static uint64_t capacity,last_lba=UINT64_MAX,pending_lba=UINT64_MAX;
static uint32_t admin_tail,admin_head,io_tail,io_head,pending_mask;
static uint16_t admin_phase=1,io_phase=1,next_cid=1;
static int online,writable;
static uint64_t write_first,write_end;
static uint8_t cache[4096],pending[4096];

static void barrier(void){__atomic_thread_fence(__ATOMIC_SEQ_CST);}
static uint32_t r32(uint32_t off){return *(volatile uint32_t*)((uint64_t)boot->device_io+off);}
static uint64_t r64(uint32_t off){return *(volatile uint64_t*)((uint64_t)boot->device_io+off);}
static void w32(uint32_t off,uint32_t value){*(volatile uint32_t*)((uint64_t)boot->device_io+off)=value;}
static void w64(uint32_t off,uint64_t value){*(volatile uint64_t*)((uint64_t)boot->device_io+off)=value;}
static void doorbell(uint32_t qid,int cq,uint32_t value){w32(DOORBELL+(2u*qid+(cq?1u:0))*stride,value);}

static int wait_status(uint32_t bit,int set,uint64_t ticks){
 uint64_t deadline=sf_ticks()+ticks;
 while(((r32(0x1c)&bit)?1:0)!=set){if(sf_ticks()>=deadline)return SF_TIMEOUT;sf_yield();}
 return 0;
}

static int completion(uint32_t base,uint32_t*head,uint16_t*phase,uint32_t cid,uint32_t*result,int cq,uint32_t qid){
 uint64_t deadline=sf_ticks()+5000;
 for(;;){
  volatile uint8_t*e=(void*)(SF_DMA+base+(*head%QDEPTH)*16);
  uint16_t status=*(uint16_t*)(e+14);
  if((status&1u)==*phase){
   uint16_t got=*(uint16_t*)(e+12);uint8_t sc=(uint8_t)((status>>1)&0xff);
   *result=*(uint32_t*)e;
   *head=(*head+1)%QDEPTH;if(*head==0)*phase^=1;
   doorbell(qid,cq,*head);
   if(got!=cid){sf_log("[NVME] stale cid got=");sf_number(got);sf_log(" want=");sf_number(cid);sf_log("\n");return SF_STALE_HANDLE;}
   if(sc){sf_log("[NVME] completion status=");sf_number(status);sf_log(" cid=");sf_number(got);sf_log("\n");return SF_DEVICE_GONE;}
   return 0;
  }
  if(sf_ticks()>=deadline)return SF_TIMEOUT;
  sf_yield();
 }
}

static int admin(uint8_t op,uint32_t nsid,uint32_t cdw10,uint32_t cdw11,uint64_t prp1,uint32_t*result){
 uint32_t cid=next_cid++;next_cid&=0xffffu;
 uint8_t*c=(void*)(SF_DMA+ADMIN_SQ+(admin_tail%QDEPTH)*64);
 memset(c,0,64);*(uint32_t*)c=op|((cid&0xffffu)<<16);*(uint32_t*)(c+4)=nsid;
 *(uint64_t*)(c+24)=prp1;*(uint32_t*)(c+40)=cdw10;*(uint32_t*)(c+44)=cdw11;
 barrier();admin_tail=(admin_tail+1)%QDEPTH;doorbell(0,0,admin_tail);
 return completion(ADMIN_CQ,&admin_head,&admin_phase,cid,result,1,0);
}

static int io(uint8_t op,uint64_t lba,int write){
 uint32_t cid=next_cid++;next_cid&=0xffffu;
 uint8_t*c=(void*)(SF_DMA+IO_SQ+(io_tail%QDEPTH)*64);
 memset(c,0,64);*(uint32_t*)c=op|((cid&0xffffu)<<16);*(uint32_t*)(c+4)=1;
 if(op!=0x00){*(uint64_t*)(c+24)=boot->dma_physical+DATA_OFF;*(uint32_t*)(c+40)=(uint32_t)lba;*(uint32_t*)(c+44)=(uint32_t)(lba>>32);*(uint32_t*)(c+48)=0;}
 (void)write;
 barrier();io_tail=(io_tail+1)%QDEPTH;doorbell(1,0,io_tail);
 uint32_t ignored;return completion(IO_CQ,&io_head,&io_phase,cid,&ignored,1,1);
}

static int read_block(uint64_t lba){
 if(last_lba==lba)return 0;
 int r=io(0x02,lba,0);if(!r){memcpy(cache,(void*)(SF_DMA+DATA_OFF),lba_bytes);last_lba=lba;}return r;
}

static int write_block(uint64_t lba){
 memcpy((void*)(SF_DMA+DATA_OFF),pending,lba_bytes);int r=io(0x01,lba,1);if(!r)last_lba=UINT64_MAX;return r;
}

static int stop(void){online=0;last_lba=UINT64_MAX;w32(0x14,r32(0x14)&~1u);return sf_sys(SYS_DEVICE_RELEASE,0,0,0);}

static int start(void){
 if(!boot->device_io||boot->device_io!=SF_DEVICE_MMIO||boot->dma_virtual!=SF_DMA||!boot->dma_physical||(boot->dma_physical&4095))return SF_DENIED;
 uint64_t cap=r64(0);if((uint32_t)(cap&0xffffu)<QDEPTH-1)return SF_DENIED;
 dstrd=(uint32_t)((cap>>32)&0xfu);stride=4u<<dstrd;
 w32(0x14,r32(0x14)&~1u);if(wait_status(1,0,100)){sf_log("[NVME] disable timeout csts=");sf_number(r32(0x1c));sf_log("\n");return SF_DEVICE_GONE;}
 w32(0x0c,0xffffffffu);
 uint32_t aqa=(QDEPTH-1)|((QDEPTH-1)<<16);w32(0x24,aqa);
 w64(0x28,boot->dma_physical+ADMIN_SQ);w64(0x30,boot->dma_physical+ADMIN_CQ);
 w32(0x14,(6u<<16)|(4u<<20)|1u);if(wait_status(1,1,1000)){sf_log("[NVME] enable timeout csts=");sf_number(r32(0x1c));sf_log("\n");return SF_DEVICE_GONE;}
 uint32_t result=0;int r=admin(0x06,1,0,0,boot->dma_physical+IDENT_OFF,&result);if(r){sf_log("[NVME] identify failed code=");sf_number(-r);sf_log(" sc=");sf_number(result);sf_log("\n");return r;}
 uint64_t nsze=*(uint64_t*)(SF_DMA+IDENT_OFF);
 uint8_t nlba=*(uint8_t*)(SF_DMA+IDENT_OFF+25),flbas=*(uint8_t*)(SF_DMA+IDENT_OFF+26)&0xf;
 if(flbas>=nlba){sf_log("[NVME] bad flbas\n");return SF_BAD_FORMAT;}
 uint32_t lf=*(uint32_t*)(SF_DMA+IDENT_OFF+128+4*flbas);uint32_t lbads=(lf>>16)&0xffu;
 if(lbads<9||lbads>12||!nsze){sf_log("[NVME] bad lba format lbads=");sf_number(lbads);sf_log(" nsze=");sf_number(nsze);sf_log("\n");return SF_BAD_FORMAT;}
 lba_bytes=1u<<lbads;ratio=lba_bytes/512;total_halves=lba_bytes/256;
 if(nsze>UINT64_MAX/ratio)return SF_BAD_FORMAT;capacity=nsze*ratio;
 /* CDW10: QID 在低 16 位，QSIZE 在高 16 位；CDW11: PC/IEN/IV。 */
 r=admin(0x05,0,1u|((QDEPTH-1)<<16),1u,boot->dma_physical+IO_CQ,&result);if(r){sf_log("[NVME] create cq failed code=");sf_number(-r);sf_log("\n");return r;}
 r=admin(0x01,0,1u|((QDEPTH-1)<<16),1u|(1u<<16),boot->dma_physical+IO_SQ,&result);if(r){sf_log("[NVME] create sq failed code=");sf_number(-r);sf_log("\n");return r;}
 writable=(boot->device_rights&SF_DEVICE_WRITE)!=0;
 if(!writable)return SF_READ_ONLY; /* 让 sf-init 使用写授权重试 */
 online=1;
 sf_log("[NVME] disk online, read-write\n");
 return 0;
}

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
  else if(m.h.opcode==OP_BLOCK_FLUSH){if(!writable||!write_end)result=SF_READ_ONLY;else if(pending_mask&&pending_mask!=((1u<<total_halves)-1))result=SF_INVALID;else{pending_mask=0;pending_lba=UINT64_MAX;result=io(0x00,0,0);}}
  else if(m.h.opcode==OP_BLOCK_WRITE){
   if(!writable||!write_end)result=SF_READ_ONLY;
   else if(!sf_block_range(capacity,m.offset,m.mode,m.length)||m.offset/512<write_first||m.offset/512>=write_end){pending_mask=0;pending_lba=UINT64_MAX;result=SF_INVALID;}
   else{
    uint64_t sector=m.offset/512,lba=sector/ratio;uint32_t off=(uint32_t)(sector%ratio)*512+m.mode;
    if(pending_lba!=lba){if(pending_mask){result=SF_INVALID;}else{pending_lba=lba;pending_mask=0;}}
    if(!result&&(off>>8)!=(uint32_t)__builtin_popcount(pending_mask)){pending_mask=0;pending_lba=UINT64_MAX;result=SF_INVALID;}
    if(!result){memcpy(pending+off,m.data,256);pending_mask|=1u<<(off>>8);
     if(pending_mask==((1u<<total_halves)-1)){result=write_block(lba);pending_mask=0;pending_lba=UINT64_MAX;}}
   }
  }
  else if(m.h.opcode==OP_BLOCK_READ){if(!sf_block_range(capacity,m.offset,m.mode,m.length))result=SF_INVALID;else if(pending_mask)result=SF_INVALID;else{uint64_t sector=m.offset/512,lba=sector/ratio;result=read_block(lba);if(!result)memcpy(m.data,cache+(uint32_t)(sector%ratio)*512+m.mode,256);}}
  else result=SF_INVALID;
  m.status=result;if(result)m.length=0;sf_send(a->manager,&m);
 }
}
