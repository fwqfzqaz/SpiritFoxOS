#include "kernel.h"
/* xHCI 主机控制器与 USB HID 输入。
 * 键盘字符进入控制台输入环形缓冲，鼠标位移复用 PS/2 指针的加速与快照
 * 环形缓冲，因此 PID 1 与 sf-wm 不需要新的 IPC 协议。控制器寄存器位于
 * 3..4 GiB 的共享 MMIO 窗口，DMA 结构放在恒等映射的内核静态区；启用
 * VT-d 时整块 64 KiB 通过 iommu_grant 映射成设备可见的 IOVA。 */
#define USB_MAX_PORTS 32
#define USB_MAX_DEVS 2
#define USB_DMA_BYTES 65536u
#define USB_RING_ENTRIES 64u
#define USB_SPIN 200000u

#define DMA_DCBAAP 0x0000u
#define DMA_CMD_RING 0x0400u
#define DMA_EVT_RING 0x0800u
#define DMA_ERST 0x0C00u
#define DMA_SCRATCH_ARRAY 0x0C40u
#define DMA_INPUT_CTX 0x1000u
#define DMA_DEV_CTX(i) (0x2000u+(i)*0x1000u)
#define DMA_EP0_RING(i) (0x4000u+(i)*0x800u)
#define DMA_INT_RING(i) (0x5000u+(i)*0x800u)
#define DMA_CTRL_BUF 0x6000u
#define DMA_REPORT(i) (0x6400u+(i)*0x100u)
#define DMA_SCRATCH(i) (0x7000u+(i)*0x1000u)
#define DMA_MAX_SCRATCH 9u

/* PORTSC */
#define PORT_CCS (1u<<0)
#define PORT_PED (1u<<1)
#define PORT_PR (1u<<4)
#define PORT_PP (1u<<9)
#define PORT_LWS (1u<<16)
#define PORT_RW1C 0x00FE0000u
#define PORT_PRC (1u<<21)

/* TRB 类型 */
#define TRB_NORMAL 1u
#define TRB_SETUP 2u
#define TRB_DATA 3u
#define TRB_STATUS 4u
#define TRB_LINK 6u
#define TRB_ENABLE_SLOT 9u
#define TRB_DISABLE_SLOT 10u
#define TRB_ADDRESS_DEVICE 11u
#define TRB_CONFIGURE_ENDPOINT 12u
#define TRB_TRANSFER 32u
#define TRB_COMPLETION 33u
#define TRB_PORT_STATUS 34u

typedef struct {uint32_t d0,d1,d2,d3;} Trb;

typedef struct {
 int used;
 uint8_t port,slot,speed,kind; /* kind: 1=键盘 2=鼠标 */
 uint8_t iface,ep_addr,dci,maxpkt,binterval;
 uint32_t report_off,ring_off,ep0_off;
 uint8_t prev_keys[6];
 unsigned ep0_idx,ep0_cycle,int_idx,int_cycle;
} UsbDev;

static uint8_t usb_dma[USB_DMA_BYTES] __attribute__((aligned(4096)));
static uint64_t usb_dma_bus;
static volatile uint8_t *usb_mmio;
static volatile uint8_t *usb_op,*usb_rt;
static volatile uint32_t *usb_db;
static uint8_t usb_max_ports,usb_enabled_slots,usb_ctx_size;
static uint32_t usb_max_scratch;
static int usb_ready;
static unsigned cmd_idx,cmd_cycle,evt_idx,evt_cycle;
static uint32_t port_change;
static UsbDev usb_devs[USB_MAX_DEVS];

static volatile int wait_cmd_done,wait_xfer_done;
static uint32_t wait_cmd_code,wait_cmd_slot,wait_xfer_code,wait_xfer_residual;
static uint64_t wait_cmd_ptr,wait_xfer_ptr;

static void xhci_event_loop(void);
static void usb_arm_interrupt(UsbDev*);
static void usb_hid_report(UsbDev*,uint32_t);
static UsbDev *dev_by_slot(uint32_t);
static void usb_enumerate(unsigned);

static inline void outl(uint16_t p,uint32_t v){__asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p));}
static inline uint32_t inl(uint16_t p){uint32_t v;__asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p));return v;}
static uint32_t usb_pci_read(uint32_t bdf,uint32_t off){outl(0xcf8,0x80000000u|(bdf<<8)|(off&0xfc));return inl(0xcfc);}
static void usb_pci_write(uint32_t bdf,uint32_t off,uint32_t v){outl(0xcf8,0x80000000u|(bdf<<8)|(off&0xfc));outl(0xcfc,v);}
static void unum(uint64_t v){
 char b[21];unsigned n=0;
 if(!v){serial("0");return;}
 while(v&&n<sizeof(b)){b[n++]=(char)('0'+v%10u);v/=10u;}
 while(n)serial((char[]){b[--n],0});
}
static inline uint32_t op_read(uint32_t off){return *(volatile uint32_t*)(usb_op+off);}
static inline void op_write(uint32_t off,uint32_t v){*(volatile uint32_t*)(usb_op+off)=v;}
static inline void op_write64(uint32_t off,uint64_t v){*(volatile uint64_t*)(usb_op+off)=v;}
static inline void rt_write(uint32_t off,uint32_t v){*(volatile uint32_t*)(usb_rt+off)=v;}
static inline void rt_write64(uint32_t off,uint64_t v){*(volatile uint64_t*)(usb_rt+off)=v;}
static inline volatile uint32_t *port_reg(unsigned port){return(volatile uint32_t*)(usb_op+0x400u+(port-1u)*0x10u);}
static inline uint64_t dma_bus(uint32_t off){return usb_dma_bus+off;}
static inline void cpu_relax(void){__asm__ volatile("pause");}

static void ring_link(uint32_t off,unsigned count,unsigned cycle){
 Trb*t=(Trb*)(usb_dma+off)+count-1u;
 uint64_t bus=dma_bus(off);
 t->d0=(uint32_t)bus;t->d1=(uint32_t)(bus>>32);t->d2=0;
 t->d3=(TRB_LINK<<10)|(1u<<1)|(cycle&1u);
}
static uint64_t ring_enqueue(uint32_t off,unsigned*idx,unsigned*cycle,unsigned count,
                             uint32_t d0,uint32_t d1,uint32_t d2,uint32_t d3){
 if(*idx>=count-1u){ring_link(off,count,*cycle);*idx=0;*cycle^=1u;}
 Trb*t=(Trb*)(usb_dma+off)+(*idx);
 uint64_t bus=dma_bus(off)+(*idx)*16u;
 t->d0=d0;t->d1=d1;t->d2=d2;t->d3=d3|(*cycle&1u);
 (*idx)++;
 return bus;
}
static void ring_reset(uint32_t off,unsigned*idx,unsigned*cycle){
 memset(usb_dma+off,0,USB_RING_ENTRIES*16u);
 ring_link(off,USB_RING_ENTRIES,1);
 *idx=0;*cycle=1;
}
static void port_clear_changes(unsigned port){
 volatile uint32_t*p=port_reg(port);
 uint32_t v=*p;
 *p=(v&(PORT_PP|0xC000u))|PORT_PP|PORT_RW1C;
}
static int port_reset(unsigned port,uint32_t*speed){
 volatile uint32_t*p=port_reg(port);
 uint32_t v=*p;
 if(!(v&PORT_CCS))return SF_DEVICE_GONE;
 *p=(v&(PORT_PP|0xC000u))|PORT_PP|PORT_PR|PORT_LWS;
 for(unsigned i=0;i<USB_SPIN;i++){
  v=*p;
  if(v&PORT_PRC)break;
  cpu_relax();
 }
 *p=(v&(PORT_PP|0xC000u))|PORT_PP|PORT_RW1C;
 v=*p;
 if(!(v&PORT_CCS))return SF_DEVICE_GONE;
 *speed=(v>>10)&0xfu;
 return 0;
}
static UsbDev *dev_by_slot(uint32_t slot){
 for(unsigned i=0;i<USB_MAX_DEVS;i++)if(usb_devs[i].used&&usb_devs[i].slot==slot)return&usb_devs[i];
 return 0;
}
static UsbDev *dev_by_port(unsigned port){
 for(unsigned i=0;i<USB_MAX_DEVS;i++)if(usb_devs[i].used&&usb_devs[i].port==port)return&usb_devs[i];
 return 0;
}
static UsbDev *dev_alloc(void){
 for(unsigned i=0;i<USB_MAX_DEVS;i++)if(!usb_devs[i].used)return&usb_devs[i];
 return 0;
}

static void xhci_event_loop(void){
 int any=0;
 for(unsigned n=0;n<128;n++){
  volatile Trb*ev=(volatile Trb*)(usb_dma+DMA_EVT_RING)+evt_idx;
  if((ev->d3&1u)!=evt_cycle)break;
  uint32_t type=(ev->d3>>10)&0x3fu;
  uint64_t ptr=((uint64_t)ev->d1<<32)|ev->d0;
  if(type==TRB_COMPLETION){
   wait_cmd_code=(ev->d2>>24)&0xffu;
   wait_cmd_slot=(ev->d3>>24)&0xffu;
   if(ptr==wait_cmd_ptr)wait_cmd_done=1;
  }else if(type==TRB_TRANSFER){
   uint32_t code=(ev->d2>>24)&0xffu;
   uint32_t residual=ev->d2&0xffffffu;
   uint32_t ep=(ev->d3>>16)&0xffu;
   if(ep==1u){
    if(ptr==wait_xfer_ptr){wait_xfer_code=code;wait_xfer_residual=residual;wait_xfer_done=1;}
   }else{
    UsbDev*d=dev_by_slot((ev->d3>>24)&0xffu);
    if(d&&ep==d->dci){
     uint32_t len=d->maxpkt;
     if(residual<len)len-=residual;else len=0;
     usb_hid_report(d,len);
    }
   }
  }else if(type==TRB_PORT_STATUS){
   uint32_t port=(ev->d0>>24)&0xffu;
   if(port&&port<=USB_MAX_PORTS)port_change|=1u<<port;
  }
  evt_idx++;if(evt_idx>=USB_RING_ENTRIES){evt_idx=0;evt_cycle^=1u;}
  any=1;
 }
 if(any){
  uint64_t erdp=dma_bus(DMA_EVT_RING)+evt_idx*16u;
  rt_write64(0x38u,erdp|8u);
  op_write(0x04u,1u<<3);
 }
}
static int xhci_cmd(uint32_t type,uint32_t d0,uint32_t d1,uint32_t d2,uint32_t d3,uint32_t*out_slot){
 wait_cmd_done=0;wait_cmd_code=0;wait_cmd_slot=0;
 uint64_t bus=ring_enqueue(DMA_CMD_RING,&cmd_idx,&cmd_cycle,USB_RING_ENTRIES,
                           d0,d1,d2,d3|(type<<10));
 wait_cmd_ptr=bus;
 __asm__ volatile("" ::: "memory");
 usb_db[0]=0;
 for(unsigned i=0;i<USB_SPIN;i++){
  xhci_event_loop();
  if(wait_cmd_done)break;
  cpu_relax();
 }
 if(!wait_cmd_done)return SF_TIMEOUT;
 if(out_slot)*out_slot=wait_cmd_slot;
 if(wait_cmd_code!=1u){serial("[USB] cmd=");unum(type);serial(" code=");unum(wait_cmd_code);serial("\n");}
 return wait_cmd_code==1u?0:SF_INVALID;
}

static char usb_key_ascii(uint8_t key,int shift){
 switch(key){
  case 0x04:return shift?'A':'a';case 0x05:return shift?'B':'b';case 0x06:return shift?'C':'c';
  case 0x07:return shift?'D':'d';case 0x08:return shift?'E':'e';case 0x09:return shift?'F':'f';
  case 0x0a:return shift?'G':'g';case 0x0b:return shift?'H':'h';case 0x0c:return shift?'I':'i';
  case 0x0d:return shift?'J':'j';case 0x0e:return shift?'K':'k';case 0x0f:return shift?'L':'l';
  case 0x10:return shift?'M':'m';case 0x11:return shift?'N':'n';case 0x12:return shift?'O':'o';
  case 0x13:return shift?'P':'p';case 0x14:return shift?'Q':'q';case 0x15:return shift?'R':'r';
  case 0x16:return shift?'S':'s';case 0x17:return shift?'T':'t';case 0x18:return shift?'U':'u';
  case 0x19:return shift?'V':'v';case 0x1a:return shift?'W':'w';case 0x1b:return shift?'X':'x';
  case 0x1c:return shift?'Y':'y';case 0x1d:return shift?'Z':'z';
  case 0x1e:return shift?'!':'1';case 0x1f:return shift?'@':'2';case 0x20:return shift?'#':'3';
  case 0x21:return shift?'$':'4';case 0x22:return shift?'%':'5';case 0x23:return shift?'^':'6';
  case 0x24:return shift?'&':'7';case 0x25:return shift?'*':'8';case 0x26:return shift?'(':'9';
  case 0x27:return shift?')':'0';
  case 0x28:return '\r';case 0x29:return 0x1b;case 0x2a:return 8;case 0x2b:return '\t';
  case 0x2c:return ' ';case 0x2d:return shift?'_':'-';case 0x2e:return shift?'+':'=';
  case 0x2f:return shift?'{':'[';case 0x30:return shift?'}':']';case 0x31:return shift?'|':'\\';
  case 0x33:return shift?':':';';case 0x34:return shift?'"':'\'';case 0x35:return shift?'~':'`';
  case 0x36:return shift?'<':',';case 0x37:return shift?'>':'.';case 0x38:return shift?'?':'/';
  default:return 0;
 }
}
static void usb_keyboard_report(UsbDev*d,const uint8_t*report,uint32_t len){
 if(len<8u)return;
 int shift=(report[0]&0x22u)!=0;
 for(unsigned i=0;i<6u;i++){
  uint8_t key=report[2+i];
  if(!key)continue;
  int seen=0;
  for(unsigned j=0;j<6u;j++)if(d->prev_keys[j]==key)seen=1;
  if(!seen){
   char c=usb_key_ascii(key,shift);
   if(c)console_input_push((uint8_t)c);
  }
 }
 memcpy(d->prev_keys,report+2,6);
}
static void usb_arm_interrupt(UsbDev*d){
 uint64_t buf=dma_bus(d->report_off);
 ring_enqueue(d->ring_off,&d->int_idx,&d->int_cycle,USB_RING_ENTRIES,
              (uint32_t)buf,(uint32_t)(buf>>32),d->maxpkt,(TRB_NORMAL<<10)|(1u<<5));
 __asm__ volatile("" ::: "memory");
 usb_db[d->slot]=d->dci;
}
static void usb_hid_report(UsbDev*d,uint32_t len){
 uint8_t*report=usb_dma+d->report_off;
 if(d->kind==1u)usb_keyboard_report(d,report,len);
 else if(d->kind==2u&&len>=3u)ps2_pointer_report((int8_t)report[1],-(int8_t)report[2],report[0]&7u);
 usb_arm_interrupt(d);
}

static long usb_control(UsbDev*d,uint8_t rt,uint8_t req,uint16_t value,uint16_t index,uint16_t len,void*out){
 if(len>0x400u)return -SF_INVALID;
 int in=(rt&0x80u)!=0;
 uint32_t trt=len?(in?3u:2u):0u;
 uint32_t s0=(uint32_t)rt|((uint32_t)req<<8)|((uint32_t)value<<16);
 uint32_t s1=(uint32_t)index|((uint32_t)len<<16);
 ring_enqueue(d->ep0_off,&d->ep0_idx,&d->ep0_cycle,USB_RING_ENTRIES,
              s0,s1,8u,(TRB_SETUP<<10)|(1u<<6)|(trt<<16));
 if(len){
  uint64_t buf=dma_bus(DMA_CTRL_BUF);
  ring_enqueue(d->ep0_off,&d->ep0_idx,&d->ep0_cycle,USB_RING_ENTRIES,
               (uint32_t)buf,(uint32_t)(buf>>32),len,(TRB_DATA<<10)|((in?1u:0u)<<16));
 }
 uint64_t status=ring_enqueue(d->ep0_off,&d->ep0_idx,&d->ep0_cycle,USB_RING_ENTRIES,
                              0,0,0,(TRB_STATUS<<10)|((in?0u:1u)<<16)|(1u<<5));
 wait_xfer_done=0;wait_xfer_code=0;wait_xfer_residual=0;wait_xfer_ptr=status;
 __asm__ volatile("" ::: "memory");
 usb_db[d->slot]=1;
 for(unsigned i=0;i<USB_SPIN;i++){
  xhci_event_loop();
  if(wait_xfer_done)break;
  cpu_relax();
 }
 if(!wait_xfer_done)return -SF_TIMEOUT;
 if(wait_xfer_code!=1u&&wait_xfer_code!=13u)return -SF_INVALID;
 if(out){
  uint32_t got=len;
  if(wait_xfer_residual<=len)got=len-wait_xfer_residual;
  memcpy(out,usb_dma+DMA_CTRL_BUF,got);
  return(long)got;
 }
 return 0;
}
static long usb_get_descriptor(UsbDev*d,uint8_t type,uint8_t index,uint16_t len,void*out){
 return usb_control(d,0x80u,6u,(uint16_t)((type<<8)|index),0,len,out);
}
static void usb_input_context(uint32_t off){
 memset(usb_dma+off,0,0x1000u);
}
static void usb_address_context(UsbDev*d,unsigned di,uint32_t maxpkt){
 usb_input_context(DMA_INPUT_CTX);
 uint32_t*icc=(uint32_t*)(usb_dma+DMA_INPUT_CTX);
 icc[1]=(1u<<0)|(1u<<1);
 uint32_t*slot=(uint32_t*)(usb_dma+DMA_INPUT_CTX+usb_ctx_size);
 slot[0]=((uint32_t)d->speed<<20)|(1u<<27);
 slot[1]=(uint32_t)d->port<<16;
 uint32_t*ep0=(uint32_t*)(usb_dma+DMA_INPUT_CTX+usb_ctx_size*2u);
 ep0[0]=0;
 ep0[1]=(3u<<1)|(4u<<3)|(maxpkt<<16);
 uint64_t ring=dma_bus(DMA_EP0_RING(di));
 ep0[2]=(uint32_t)(ring|1u);
 ep0[3]=(uint32_t)(ring>>32);
 ep0[4]=8;
}
static void usb_config_context(UsbDev*d,unsigned di){
 usb_input_context(DMA_INPUT_CTX);
 uint32_t*icc=(uint32_t*)(usb_dma+DMA_INPUT_CTX);
 icc[1]=(1u<<0)|(1u<<d->dci);
 uint32_t*slot=(uint32_t*)(usb_dma+DMA_INPUT_CTX+usb_ctx_size);
 slot[0]=((uint32_t)d->speed<<20)|((uint32_t)d->dci<<27);
 slot[1]=(uint32_t)d->port<<16;
 uint32_t*ep=(uint32_t*)(usb_dma+DMA_INPUT_CTX+usb_ctx_size*(1u+d->dci));
 ep[0]=(uint32_t)d->binterval<<16;
 ep[1]=(3u<<1)|(7u<<3)|((uint32_t)d->maxpkt<<16);
 uint64_t ring=dma_bus(DMA_INT_RING(di));
 ep[2]=(uint32_t)(ring|1u);
 ep[3]=(uint32_t)(ring>>32);
 ep[4]=d->maxpkt;
}
static int usb_find_hid(UsbDev*d,const uint8_t*cfg,uint16_t total){
 unsigned off=0;int in_iface=0;
 while(off+2u<=total){
  uint8_t bl=cfg[off],type=cfg[off+1];
  if(bl<2u||off+bl>total)break;
  if(type==4u&&bl>=9u){
   uint8_t cls=cfg[off+5],sub=cfg[off+6],proto=cfg[off+7];
   in_iface=(cls==3u&&sub==1u&&(proto==1u||proto==2u));
   if(in_iface){d->iface=cfg[off+2];d->kind=proto;}
  }else if(type==5u&&bl>=7u&&in_iface){
   uint8_t addr=cfg[off+2],attr=cfg[off+3];
   uint16_t mps=(uint16_t)(cfg[off+4]|((uint16_t)cfg[off+5]<<8));
   if((attr&3u)==3u&&(addr&0x80u)){
    d->ep_addr=addr;d->maxpkt=(uint8_t)(mps&0x7ffu);d->binterval=cfg[off+6];
    if(d->maxpkt>64u)d->maxpkt=64u;
    if(!d->maxpkt)d->maxpkt=8u;
    return 1;
   }
  }
  off+=bl;
 }
 return 0;
}
static void usb_release(UsbDev*d){
 if(!d->used)return;
 if(d->slot){
  xhci_cmd(TRB_DISABLE_SLOT,0,0,0,(uint32_t)d->slot<<24,0);
  uint64_t*dcbaa=(uint64_t*)(usb_dma+DMA_DCBAAP);
  if(d->slot<64u)dcbaa[d->slot]=0;
 }
 memset(d,0,sizeof(*d));
}
static void usb_enumerate(unsigned port){
 uint32_t speed=0;
 if(port_reset(port,&speed)){serial("[USB] port reset failed\n");return;}
 if(speed!=1u&&speed!=2u&&speed!=3u){
  serial("[USB] unsupported port speed=");unum(speed);serial("\n");
  port_clear_changes(port);
  return;
 }
 uint32_t slot=0;
 int r=xhci_cmd(TRB_ENABLE_SLOT,0,0,0,0,&slot);
 if(r||!slot||slot>=64u){serial("[USB] enable slot failed\n");port_clear_changes(port);return;}
 UsbDev*d=dev_alloc();
 if(!d){
  xhci_cmd(TRB_DISABLE_SLOT,0,0,0,slot<<24,0);
  port_clear_changes(port);
  return;
 }
 unsigned di=(unsigned)(d-usb_devs);
 uint32_t dev_ctx_off=DMA_DEV_CTX(di);
 memset(usb_dma+dev_ctx_off,0,0x1000u);
 memset(d,0,sizeof(*d));
 d->used=1;d->port=(uint8_t)port;d->slot=(uint8_t)slot;d->speed=(uint8_t)speed;
 d->ring_off=DMA_INT_RING(di);d->report_off=DMA_REPORT(di);d->ep0_off=DMA_EP0_RING(di);
 uint64_t*dcbaa=(uint64_t*)(usb_dma+DMA_DCBAAP);
 dcbaa[slot]=dma_bus(dev_ctx_off);
 ring_reset(d->ep0_off,&d->ep0_idx,&d->ep0_cycle);
 ring_reset(d->ring_off,&d->int_idx,&d->int_cycle);
 uint32_t maxpkt=speed==3u?64u:8u;
 usb_address_context(d,di,maxpkt);
 uint64_t ictx=dma_bus(DMA_INPUT_CTX);
 r=xhci_cmd(TRB_ADDRESS_DEVICE,(uint32_t)ictx,(uint32_t)(ictx>>32),0,(uint32_t)slot<<24,0);
 if(r){
  serial("[USB] address device failed code=");unum((uint32_t)(-r));serial("\n");
  usb_release(d);port_clear_changes(port);return;
 }
 uint8_t desc[18];
 long n=usb_get_descriptor(d,1,0,(uint16_t)sizeof(desc),desc);
 if(n<9){
  serial("[USB] device descriptor failed\n");
  usb_release(d);port_clear_changes(port);return;
 }
 if(desc[7]>maxpkt&&desc[7]<=64u){
  /* 设备声明更大的 EP0 包长；FS 设备通常仍可接受 8 字节控制传输。 */
  serial("[USB] ep0 max packet=");unum(desc[7]);serial("\n");
 }
 uint8_t cfg[512];
 n=usb_get_descriptor(d,2,0,9,cfg);
 if(n<9){
  serial("[USB] config header failed\n");
  usb_release(d);port_clear_changes(port);return;
 }
 uint16_t total=(uint16_t)(cfg[2]|((uint16_t)cfg[3]<<8));
 if(total<9u||total>sizeof(cfg)){
  serial("[USB] bad config length\n");
  usb_release(d);port_clear_changes(port);return;
 }
 n=usb_get_descriptor(d,2,0,total,cfg);
 if(n<9||usb_find_hid(d,cfg,total)==0){
  serial("[USB] HID interface not found\n");
  usb_release(d);port_clear_changes(port);return;
 }
 uint8_t cfg_value=cfg[5];
 d->dci=(uint8_t)(((d->ep_addr&0x0fu)<<1)|((d->ep_addr&0x80u)?1u:0u));
 if(usb_control(d,0,9,cfg_value,0,0,0)<0){
  serial("[USB] set configuration failed\n");
  usb_release(d);port_clear_changes(port);return;
 }
 usb_config_context(d,di);
 r=xhci_cmd(TRB_CONFIGURE_ENDPOINT,(uint32_t)ictx,(uint32_t)(ictx>>32),0,(uint32_t)slot<<24,0);
 if(r){
  serial("[USB] configure endpoint failed\n");
  usb_release(d);port_clear_changes(port);return;
 }
 usb_control(d,0x21u,0x0au,0,(uint16_t)d->iface,0,0);
 usb_control(d,0x21u,0x0bu,0,(uint16_t)d->iface,0,0);
 usb_arm_interrupt(d);
 serial(d->kind==1u?"[USB] HID keyboard port=":"[USB] HID mouse port=");
 unum(port);serial(" slot=");unum(slot);serial(" dci=");unum(d->dci);
 serial(" maxpkt=");unum(d->maxpkt);serial("\n");
 port_clear_changes(port);
}

static uint64_t pci_bar0(uint32_t bdf,uint32_t*size){
 uint32_t lo=usb_pci_read(bdf,0x10);
 if(lo&1u)return 0;
 uint32_t type=(lo>>1)&3u;
 uint32_t hi=type==2u?usb_pci_read(bdf,0x14):0;
 uint64_t bar=((uint64_t)hi<<32)|(lo&~0x0fu);
 usb_pci_write(bdf,0x10,0xffffffffu);
 if(type==2u)usb_pci_write(bdf,0x14,0xffffffffu);
 uint32_t mlo=usb_pci_read(bdf,0x10);
 uint32_t mhi=type==2u?usb_pci_read(bdf,0x14):0;
 uint64_t mask=((uint64_t)mhi<<32)|(mlo&~0x0fu);
 usb_pci_write(bdf,0x10,lo);
 if(type==2u)usb_pci_write(bdf,0x14,hi);
 if(!mask)return 0;
 uint64_t sz=(~mask)+1u;
 *size=(uint32_t)(sz>0x100000u?0x100000u:sz);
 return bar;
}
static uint32_t find_xhci(void){
 for(uint32_t bdf=0;bdf<256u;bdf++){
  uint32_t vend=usb_pci_read(bdf,0);
  if(vend==0xffffffffu)continue;
  uint32_t cls=usb_pci_read(bdf,8);
  if(((cls>>24)&0xffu)==0x0cu&&((cls>>16)&0xffu)==0x03u&&((cls>>8)&0xffu)==0x30u)return bdf;
 }
 return 0xffffffffu;
}
static int controller_start(void){
 op_write(0x00u,op_read(0x00u)&~1u);
 for(unsigned i=0;i<USB_SPIN;i++){
  if(op_read(0x04u)&1u)break;
  cpu_relax();
 }
 op_write(0x00u,2u);
 for(unsigned i=0;i<USB_SPIN;i++){
  if(!(op_read(0x00u)&2u)&&!(op_read(0x04u)&(1u<<11)))break;
  cpu_relax();
 }
 op_write(0x04u,(1u<<2)|(1u<<3)|(1u<<4)|(1u<<12));
 return(op_read(0x00u)&2u)||(op_read(0x04u)&(1u<<11))?SF_TIMEOUT:0;
}
static int controller_structures(void){
 uint32_t hcsparams1=*(volatile uint32_t*)(usb_mmio+0x04u);
 uint32_t hcsparams2=*(volatile uint32_t*)(usb_mmio+0x08u);
 uint32_t hccparams1=*(volatile uint32_t*)(usb_mmio+0x10u);
 usb_max_ports=(uint8_t)((hcsparams1>>24)&0xffu);
 usb_ctx_size=(hccparams1&(1u<<2))?64u:32u;
 usb_max_scratch=((hcsparams2>>21)&0x1fu)|(((hcsparams2>>27)&0x1fu)<<5);
 uint32_t max_slots=hcsparams1&0xffu;
 if(!max_slots||!usb_max_ports)return SF_BAD_FORMAT;
 usb_enabled_slots=(uint8_t)(max_slots>8u?8u:max_slots);
 if(usb_max_scratch>DMA_MAX_SCRATCH){
  serial("[USB] too many scratchpads=");unum(usb_max_scratch);serial("\n");
  return SF_NO_SPACE;
 }
 op_write(0x38u,usb_enabled_slots);
 uint64_t*dcbaa=(uint64_t*)(usb_dma+DMA_DCBAAP);
 if(usb_max_scratch){
  dcbaa[0]=dma_bus(DMA_SCRATCH_ARRAY);
  uint64_t*sa=(uint64_t*)(usb_dma+DMA_SCRATCH_ARRAY);
  for(uint32_t i=0;i<usb_max_scratch;i++)sa[i]=dma_bus(DMA_SCRATCH(i));
 }
 op_write64(0x30u,dma_bus(DMA_DCBAAP));
 op_write64(0x18u,dma_bus(DMA_CMD_RING)|1u);
 uint32_t*erst=(uint32_t*)(usb_dma+DMA_ERST);
 erst[0]=(uint32_t)dma_bus(DMA_EVT_RING);
 erst[1]=(uint32_t)(dma_bus(DMA_EVT_RING)>>32);
 erst[2]=USB_RING_ENTRIES; /* 段内 TRB 数量，规范要求 16..4096 */
 erst[3]=0;
 rt_write(0x20u,0);
 rt_write(0x28u,1);
 rt_write64(0x30u,dma_bus(DMA_ERST));
 rt_write64(0x38u,dma_bus(DMA_EVT_RING)|8u);
 op_write(0x00u,1u);
 for(unsigned i=0;i<USB_SPIN;i++){
  if(!(op_read(0x04u)&1u))break;
  cpu_relax();
 }
 return(op_read(0x04u)&1u)?SF_TIMEOUT:0;
}
void usb_init(void){
 usb_ready=0;
 uint32_t bdf=find_xhci();
 if(bdf==0xffffffffu){serial("[USB] xHCI controller not found\n");return;}
 uint64_t bar=0;
 uint32_t bar_size=0;
 bar=pci_bar0(bdf,&bar_size);
 if(!bar||!bar_size||bar_size>0x200000ull){
  serial("[USB] unusable xHCI BAR\n");return;
 }
 uint32_t cmd=usb_pci_read(bdf,0x04u);
 usb_pci_write(bdf,0x04u,cmd|0x2u|0x4u);
 if(space_map_kernel_mmio(bar,bar_size,0xfc000000ull)){
  serial("[USB] MMIO mapping failed\n");return;
 }
 if(iommu_mode()==2u){serial("[USB] IOMMU unsupported; controller disabled\n");return;}
 if(iommu_mode()==1u&&iommu_grant(bdf,(uint64_t)usb_dma,&usb_dma_bus)){
  serial("[USB] IOMMU grant failed\n");return;
 }
 if(iommu_mode()!=1u)usb_dma_bus=(uint64_t)usb_dma;
 memset(usb_dma,0,sizeof(usb_dma));
 usb_mmio=(volatile uint8_t*)(uintptr_t)0xfc000000ull;
 uint32_t caplen=usb_mmio[0];
 uint32_t dboff=*(volatile uint32_t*)(usb_mmio+0x14u);
 uint32_t rtsoff=*(volatile uint32_t*)(usb_mmio+0x18u);
 if(!caplen||!dboff||!rtsoff){serial("[USB] invalid capability registers\n");return;}
 usb_op=(volatile uint8_t*)(uintptr_t)(0xfc000000ull+caplen);
 usb_db=(volatile uint32_t*)(uintptr_t)(0xfc000000ull+dboff);
 usb_rt=(volatile uint8_t*)(uintptr_t)(0xfc000000ull+rtsoff);
 uint32_t hciversion=(*(volatile uint32_t*)usb_mmio>>16)&0xffffu;
 serial("[USB] xHCI bdf=");unum(bdf);serial(" version=");
 unum(hciversion>>8);serial(".");unum(hciversion&0xffu);serial("\n");
 if(controller_start()){
  serial("[USB] controller reset timeout\n");return;
 }
 cmd_idx=0;cmd_cycle=1;evt_idx=0;evt_cycle=1;port_change=0;
 ring_reset(DMA_CMD_RING,&cmd_idx,&cmd_cycle);
 memset(usb_dma+DMA_EVT_RING,0,USB_RING_ENTRIES*16u);
 if(controller_structures()){
  serial("[USB] controller start failed\n");return;
 }
 usb_ready=1;
 serial("[USB] ports=");unum(usb_max_ports);serial(" slots=");unum(usb_enabled_slots);
 serial(" ctx=");unum(usb_ctx_size);serial("\n");
 for(unsigned port=1;port<=usb_max_ports&&port<=USB_MAX_PORTS;port++){
  volatile uint32_t*p=port_reg(port);
  uint32_t v=*p;
  *p=(v&(PORT_PP|0xC000u))|PORT_PP;
  port_clear_changes(port);
 }
 for(unsigned i=0;i<100000u;i++)cpu_relax();
 for(unsigned port=1;port<=usb_max_ports&&port<=USB_MAX_PORTS;port++){
  if(*port_reg(port)&PORT_CCS)usb_enumerate(port);
 }
}
void usb_poll(void){
 if(!usb_ready)return;
 xhci_event_loop();
 for(unsigned n=0;n<USB_MAX_PORTS&&port_change;n++){
  unsigned port=0;
  for(unsigned i=1;i<=USB_MAX_PORTS;i++)if(port_change&(1u<<i)){port=i;break;}
  if(!port)break;
  port_change&=~(1u<<port);
  if(port>usb_max_ports)continue;
  volatile uint32_t*p=port_reg(port);
  uint32_t v=*p;
  UsbDev*d=dev_by_port(port);
  if(v&PORT_CCS){
   if(!d)usb_enumerate(port);
   else port_clear_changes(port);
  }else if(d){
   serial("[USB] device removed port=");unum(port);serial("\n");
   usb_release(d);
   port_clear_changes(port);
  }else port_clear_changes(port);
 }
}
