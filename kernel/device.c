/* 设备授权内核侧：PCI 配置读取、DMA 隔离（IOMMU）、设备授予/释放与隔离区。
 * virtqueue 与块设备协议在 sfmod/ring-3 驱动内实现，内核只做硬件授权。 */
#include "kernel.h"
#define SF_DEVICE_SLOTS 2u
static uint8_t dma[SF_DEVICE_SLOTS][SF_DMA_BYTES] __attribute__((aligned(4096)));
static uint32_t slot_owner[SF_DEVICE_SLOTS],slot_bdf[SF_DEVICE_SLOTS];
/* INTx 状态：line 来自 PCI 配置 0x3C，所有设备共用固定向量 0x50。 */
static uint32_t slot_irq_enabled[SF_DEVICE_SLOTS],slot_irq_pending[SF_DEVICE_SLOTS];
static int ioapic_ready,ioapic_max_gsi;
#define IOAPIC_BASE 0xfec00000ull
#define IRQ_VECTOR 0x50u
static int quarantined;
static uint8_t guard[4096] __attribute__((aligned(4096)));
static SfDmaStatus dma_status;
static void outl(uint16_t port,uint32_t value){__asm__ volatile("outl %0,%1"::"a"(value),"Nd"(port));}
static uint32_t inl(uint16_t port){uint32_t v;__asm__ volatile("inl %1,%0":"=a"(v):"Nd"(port));return v;}
static uint32_t config_read(uint32_t bdf,uint32_t offset){outl(0xcf8,0x80000000u|(bdf<<8)|offset);return inl(0xcfc);}
static void config_write_command(uint32_t bdf,uint16_t command){outl(0xcf8,0x80000004u|(bdf<<8));__asm__ volatile("outw %0,%1"::"a"(command),"Nd"((uint16_t)0xcfc));}
long pci_read(uint64_t bdf,uint64_t offset){if(bdf>65535||offset>252||(offset&3))return SF_INVALID;return config_read(bdf,offset);}
/* 设备初始化：金丝雀页填充 0xa5（后续校验内核 DMA 区未被设备越界写穿），
 * 关闭固件遗留的总线主控 DMA，再初始化 IOMMU。 */
void device_init(SfBootInfo*b){
 memset(guard,0xa5,sizeof(guard));
 for(unsigned i=0;i<SF_DEVICE_SLOTS;i++){slot_bdf[i]=UINT32_MAX;slot_irq_enabled[i]=slot_irq_pending[i]=0;}
 /* Firmware no longer owns any bus-zero DMA engines after ExitBootServices. */
 for(unsigned bdf=0;bdf<256;bdf++)if(config_read(bdf,0)!=UINT32_MAX){uint16_t command=config_read(bdf,4);if(command&4)config_write_command(bdf,command&~4u);}
 iommu_init(b);
}
/* 设备状态快照：IOMMU 模式、当前持有者、金丝雀完整性与活动映射。 */
int device_status(SfDmaStatus*out){
 uint32_t owner=0;for(unsigned i=0;i<SF_DEVICE_SLOTS;i++)if(slot_owner[i]){owner=slot_owner[i];break;}
 dma_status.size=sizeof(dma_status);dma_status.mode=iommu_mode();dma_status.owner=owner;dma_status.probe_address=(uint64_t)guard;dma_status.guard_intact=1;dma_status.active_mapping=iommu_active();
 for(unsigned i=0;i<sizeof(guard);i++)if(guard[i]!=0xa5)dma_status.guard_intact=0;*out=dma_status;return 0;
}
/* 释放设备：复位设备、撤除总线主控与 IOMMU 授权。复位或撤销失败时进入
 * 隔离区（DMA 区永久保留不复用），返回 SF_DEVICE_GONE。 */
int device_release(Task*t){
 if(t->device_slot<SF_DEVICE_SLOTS&&slot_owner[t->device_slot]==t->pid){slot_irq_enabled[t->device_slot]=0;slot_irq_pending[t->device_slot]=0;}
 t->irq_waiting=0;
 if(!t->device_io)return t->device_fault?SF_DEVICE_GONE:0;
 /* Keep DMA storage pinned even after successful reset. Never recycle it as RAM. */
 if(t->device_modern)*(volatile uint8_t*)0xffe00014ull=0;else outb(t->device_io+18,0);
 uint16_t command=config_read(t->device_bdf,4)&0xffff;
 config_write_command(t->device_bdf,command&~4u);
 int reset=0;for(unsigned i=0;i<10000;i++)if((t->device_modern?*(volatile uint8_t*)0xffe00014ull:inb(t->device_io+18))==0){reset=1;break;}
 int invalidation=iommu_revoke(t->device_bdf);
 if((config_read(t->device_bdf,4)&4)||!reset||invalidation){quarantined=1;serial("[CORE] DMA device quarantined; pinned pages retained\n");}
 /* A released-but-live driver still maps its arena; the slot stays reserved
  * until device_forget runs when that address space is destroyed. */
 t->device_fault=quarantined;t->device_io=0;t->device_slot=UINT32_MAX;
 return quarantined?SF_DEVICE_GONE:0;
}
/* A released live driver still maps the pinned DMA arena. Only allow another
 * owner after the old address space has been destroyed, including failure paths. */
void device_forget(Task*t){
 for(unsigned i=0;i<SF_DEVICE_SLOTS;i++)if(slot_owner[i]==t->pid){slot_owner[i]=0;slot_bdf[i]=UINT32_MAX;slot_irq_enabled[i]=slot_irq_pending[i]=0;}
}
/* IOMMU 故障处理：逐条消费故障记录，定位到持有者任务时终止任务并隔离。 */
void device_poll(void){
 uint32_t source,reason;uint64_t address;
 for(unsigned fault=0;fault<64&&iommu_fault(&source,&reason,&address);fault++){
  dma_status.faults++;dma_status.source=source;dma_status.reason=reason;dma_status.last_address=address;
  serial("[IOMMU] blocked DMA source=");hex(source);serial(" address=");hex(address);serial(" reason=");hex(reason);serial("\n");
  for(unsigned i=0;i<SF_MAX_THREADS;i++){Task*t=&tasks[i];if(!t->main||t->state==T_DEAD)continue;unsigned slot=SF_DEVICE_SLOTS;for(unsigned s=0;s<SF_DEVICE_SLOTS;s++)if(slot_owner[s]==t->pid&&slot_bdf[s]==source){slot=s;break;}if(slot==SF_DEVICE_SLOTS)continue;device_release(t);quarantined=1;t->device_fault=1;task_kill(t->pid,SF_DEVICE_GONE);serial("[IOMMU] offending driver terminated and quarantined\n");break;}
 }
}
/* 设备授予：校验 BDF、厂商/设备标识（仅 virtio-block 1.0/legacy-net/modern-net）、
 * modern 设备校验 BAR4 中的 common/device/notify 能力布局，映射 MMIO 与
 * 私有 DMA 区（SF_DMA），经 IOMMU 建立总线地址后开启设备。
 * 成功时把启动参数（DMA 虚拟/物理地址、IO 端口、BDF、权限）写回 args。 */
int device_grant(Task*t,uint32_t bdf_plus_one,uint32_t rights,SfBootArgs*args){
 if(!bdf_plus_one||bdf_plus_one>65536)return SF_INVALID;
 if(quarantined)return SF_BUSY;if(iommu_mode()==2)return SF_DEVICE_GONE;
 uint32_t bdf=bdf_plus_one-1;for(unsigned i=0;i<SF_DEVICE_SLOTS;i++)if(slot_bdf[i]==bdf)return SF_BUSY;
 uint32_t identity=config_read(bdf,0);int modern=identity==0x10421af4u;int legacy_net=identity==0x10001af4u;if(identity!=0x10011af4u&&!legacy_net&&!modern)return SF_DENIED;
 if(iommu_mode()==1&&!modern)return SF_DENIED; /* Legacy virtio bypasses platform DMA. */
 serial("[DEV] bdf=");hex(bdf);serial(" line=");hex(config_read(bdf,0x3c)&0xffu);serial("\n");
 uint32_t bar=config_read(bdf,modern?32:16);uint64_t physical=0;
 if(modern){
  if(iommu_mode()!=1||(bar&7)!=4)return SF_DENIED;physical=((uint64_t)config_read(bdf,36)<<32)|(bar&~15u);if((physical&16383)||physical<0xc0000000ull||physical>=0x1000000000000ull-16384)return SF_DENIED;
  /* The validated QEMU modern profile places common/device/notify in BAR4. */
  unsigned cap=config_read(bdf,0x34)&255,found=0;for(unsigned step=0;cap&&step<48;step++){if(cap<0x40||cap>0xfc||(cap&3))return SF_BAD_FORMAT;uint32_t h=config_read(bdf,cap);if((h&255)==9){unsigned kind=h>>24,length=(h>>16)&255;if(length<16||cap>240)return SF_BAD_FORMAT;uint32_t id=config_read(bdf,cap+4)&255,off=config_read(bdf,cap+8),n=config_read(bdf,cap+12);if((kind==1||kind==2||kind==4)&&id!=4)return SF_DENIED;if(kind==1&&off==0&&n>=56)found|=1;if(kind==2&&off==0x3000&&n>=2&&length>=20&&cap<=236&&config_read(bdf,cap+16)==4)found|=2;if(kind==4&&off==0x2000&&n>=8)found|=4;}cap=(h>>8)&255;}
  if(found!=7||cap||space_device_mmio(physical))return SF_DENIED;t->device_io=SF_DEVICE_MMIO;t->device_modern=1;
 }else{if(!(bar&1)||(bar&~3u)<0x1000||(bar&~3u)>0xffc0)return SF_DENIED;t->device_io=bar&~3u;}
 t->device_bdf=bdf;
 if(device_release(t))return SF_DEVICE_GONE;
 unsigned slot=SF_DEVICE_SLOTS;for(unsigned i=0;i<SF_DEVICE_SLOTS;i++)if(!slot_owner[i]){slot=i;break;}
 if(slot==SF_DEVICE_SLOTS)return SF_BUSY;
 t->device_io=modern?SF_DEVICE_MMIO:bar&~3u;t->device_slot=slot;slot_owner[slot]=t->pid;slot_bdf[slot]=bdf;memset(dma[slot],0,sizeof(dma[slot]));
 if(modern)for(unsigned off=0;off<16384;off+=4096)if(space_map_mmio(t,SF_DEVICE_MMIO+off,physical+off)){device_release(t);return SF_NO_SPACE;}
 for(unsigned off=0;off<sizeof(dma[slot]);off+=4096)if(space_map(t,SF_DMA+off,(uint64_t)(dma[slot]+off),3)){device_release(t);return SF_NO_SPACE;}
 uint64_t bus_address;int mapped=iommu_grant(bdf,(uint64_t)dma[slot],&bus_address);if(mapped){device_release(t);return mapped;}
 uint16_t command=config_read(bdf,4)&0xffff,enable=modern?6:5;config_write_command(bdf,command|enable);
 if((config_read(bdf,4)&enable)!=enable){device_release(t);return SF_DEVICE_GONE;}
 args->dma_virtual=SF_DMA;args->dma_physical=bus_address;args->device_io=t->device_io;args->device_bdf=bdf;args->device_rights=rights|(iommu_mode()==1?SF_DEVICE_ISOLATED:0)|(modern?SF_DEVICE_MODERN:0);
 return 0;
}
/* 受限端口 IO：仅设备持有者、legacy 设备的 [io, io+64) 窗口、1/2/4 字节宽度。 */
long device_io(Task*t,uint64_t port,uint64_t operation,uint64_t value){
 unsigned width=operation&255;int write=(operation&256)!=0;
 if(operation&~0x1ffull||(width!=1&&width!=2&&width!=4))return SF_INVALID;
 if(t->device_modern||!t->device_io||t->device_slot>=SF_DEVICE_SLOTS||slot_owner[t->device_slot]!=t->pid||port<t->device_io||port>t->device_io+64u-width)return SF_DENIED;
 if(write){if(width==1)outb(port,value);else if(width==2)__asm__ volatile("outw %0,%1"::"a"((uint16_t)value),"Nd"((uint16_t)port));else outl(port,value);return 0;}
 if(width==1)return inb(port);if(width==2){uint16_t v;__asm__ volatile("inw %1,%0":"=a"(v):"Nd"((uint16_t)port));return v;}return inl(port);
}
/* IOAPIC 间接寄存器：0xFEC00000 选索引，+0x10 读写数据。 */
static uint32_t ioapic_read(uint32_t reg){*(volatile uint32_t*)IOAPIC_BASE=reg;return *(volatile uint32_t*)(IOAPIC_BASE+0x10);}
static void ioapic_write(uint32_t reg,uint32_t value){*(volatile uint32_t*)IOAPIC_BASE=reg;*(volatile uint32_t*)(IOAPIC_BASE+0x10)=value;}
/* PCI INTx 低电平有效、电平触发，固定投递 BSP。多设备共用向量 0x50，
 * 由中断处理置每槽 pending 并唤醒等待者。 */
/* 部分 q35 固件把 PIRQ 编程到 ISA 10/11 且只经 8259 投递；解除 PCI 中断线
 * 的 PIC 屏蔽（从片还需打开主片级联 IRQ2）。 */
static void pic_unmask(uint32_t line){
 if(line<8)outb(0x21,inb(0x21)&(uint8_t)~(1u<<line));
 else if(line<16){outb(0x21,inb(0x21)&(uint8_t)~4u);outb(0xa1,inb(0xa1)&(uint8_t)~(1u<<(line-8)));}
}
static void ioapic_route(uint32_t gsi){
 if(!ioapic_ready||gsi>(uint32_t)ioapic_max_gsi)return;
 /* GSI 12 是 ISA 的 PS/2 鼠标中断，按 ISA 规范配成边缘触发、高电平有效；
  * 其余 PIRQ 才是 PCI 的低电平/电平触发。用错极性会在缓冲非空时反复触发。 */
 uint32_t high=(uint32_t)cpus[0].apic_id<<24,low=IRQ_VECTOR|(gsi==12?0u:((1u<<13)|(1u<<15)));
 ioapic_write(0x10u+gsi*2u,ioapic_read(0x10u+gsi*2u)|(1u<<16));
 ioapic_write(0x11u+gsi*2u,high);
 ioapic_write(0x10u+gsi*2u,low);
}
/* 启动时建立 IOAPIC 窗口并预置常见 PIRQ 路由：q35 固件的 PIRQRC 会把
 * PIRQA-H 落到 ISA 10/11，旧式固件则落到 16..23，因此整段 9..23 都要接通。
 * 具体设备的中断线仍由 PCI 配置 0x3C 读出后再核对。 */
void device_irq_init(void){
 if(space_ioapic_init()){serial("[IRQ] IOAPIC unavailable; polling fallback\n");return;}
 uint32_t version=ioapic_read(1);ioapic_max_gsi=(int)((version>>16)&255u);
 if(ioapic_max_gsi<23){serial("[IRQ] IOAPIC entries insufficient; polling fallback\n");return;}
 ioapic_ready=1;for(unsigned gsi=9;gsi<=23;gsi++)ioapic_route(gsi);
 serial("[IRQ] IOAPIC ready vector=");hex(IRQ_VECTOR);serial("\n");
}
/* 驱动取得当前授权设备的中断：清除 PCIe INTx Disable，按配置 0x3C 补写
 * IOAPIC 重定向项，并解除整段 PCI 中断线的 8259 屏蔽。 */
int device_irq_acquire(Task*t){
 Task*m=task_main(t);
 if(m->device_slot>=SF_DEVICE_SLOTS||slot_owner[m->device_slot]!=m->pid||slot_bdf[m->device_slot]==UINT32_MAX)return SF_DENIED;
 /* PCIe 设备的 INTx Disable 位置位时先清除，否则电平中断永不投递。 */
 uint16_t command=config_read(m->device_bdf,4)&0xffff;
 if(command&(1u<<10))config_write_command(m->device_bdf,command&~(1u<<10));
 uint32_t line=config_read(m->device_bdf,0x3c)&255u;
 if(line<24)ioapic_route(line);
 /* 固件写入的 line 可能与实际 PIRQ 映射不一致（q35 PIRQRC 常把 PIRQ
  * 落到 ISA 10/11），因此整段 PCI 中断线同时解除 PIC 屏蔽。 */
 for(unsigned l=9;l<16;l++)pic_unmask(l);
 serial("[IRQ] device line=");hex(line);serial("\n");
 slot_irq_pending[m->device_slot]=0;slot_irq_enabled[m->device_slot]=1;t->irq_waiting=0;
 return (line<16||ioapic_ready)?(int)line:SF_DEVICE_GONE;
}
int device_irq_release(Task*t){
 Task*m=task_main(t);
 if(m->device_slot>=SF_DEVICE_SLOTS||slot_owner[m->device_slot]!=m->pid)return SF_DENIED;
 slot_irq_enabled[m->device_slot]=0;slot_irq_pending[m->device_slot]=0;t->irq_waiting=0;return 0;
}
/* 等待一次中断：已有 pending 立即消费；否则登记等待并交给调度器阻塞，
 * timeout 为 0 时无限等待，否则由 ipc_deadlines 在超时后唤醒。 */
int device_irq_wait(Task*t,uint32_t timeout){
 Task*m=task_main(t);
 if(m->device_slot>=SF_DEVICE_SLOTS||slot_owner[m->device_slot]!=m->pid||!slot_irq_enabled[m->device_slot])return SF_DENIED;
 if(slot_irq_pending[m->device_slot]){slot_irq_pending[m->device_slot]=0;return 0;}
 t->irq_waiting=1;t->wait_deadline=timeout?(ticks+(uint64_t)timeout)*10000000ull:0;
 return SF_WOULD_BLOCK;
}
/* 中断处理：置位所有已启用槽并唤醒等待线程。Legacy virtio 的 ISR 读操作
 * 清除电平中断源；modern 设备暂不注册 INTx（保持轮询）。 */
void device_irq_vector(void){
 for(unsigned s=0;s<SF_DEVICE_SLOTS;s++){
  if(!slot_irq_enabled[s])continue;
  slot_irq_pending[s]=1;
  if(slot_owner[s])for(unsigned i=0;i<SF_MAX_THREADS;i++)if(tasks[i].main&&tasks[i].pid==slot_owner[s]){if(!tasks[i].device_modern&&tasks[i].device_io)inb(tasks[i].device_io+19);break;}
 }
 for(unsigned i=0;i<SF_MAX_THREADS;i++){
  Task*t=&tasks[i];
  if(t->state==T_BLOCKED&&t->irq_waiting){t->irq_waiting=0;t->wait_deadline=0;t->frame->rax=0;task_wake(t);}
 }
}
