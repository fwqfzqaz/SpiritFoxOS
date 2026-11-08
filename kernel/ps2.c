#include "kernel.h"
/* PS/2 指针中断驱动：i8042 以 IRQ12 通知新数据，内核在中断里排空输出缓冲、
 * 解析 3 字节包并把采样快照压进环形缓冲；PID1 经 SYS_POINTER_READ 只弹出
 * 快照，不再依赖 10ms 定时器节拍，快速滑动也不会因 i8042 队列满而丢位移。
 * 坐标在这里夹紧到 framebuffer 范围，合成器只做命中测试与绘制。 */
#define PS2_DATA 0x60
#define PS2_COMMAND 0x64
#define PS2_STATUS_OBF 0x01
#define PS2_STATUS_IBF 0x02
#define PS2_STATUS_AUX 0x20
/* 16 个快照足够覆盖两个定时器周期的突发；快照是绝对坐标，满了丢最旧，
 * 最新位置始终保留。 */
#define PS2_RING 16
static uint8_t packet[3];
static unsigned packet_index;
static int32_t pointer_x,pointer_y;
static uint8_t pointer_buttons;
static uint32_t pointer_width,pointer_height;
static int pointer_ready,irq_logged;
static uint32_t ring_states[PS2_RING];
static unsigned ring_head,ring_tail,ring_count;
/* 指针加速：niri 自身不做滤波，它把 accel-profile=adaptive / accel-speed
 * 交给 libinput；libinput 按移动速度给位移加自适应增益（慢速保持精确，
 * 快速按速度放大）。这里在采样点复刻同一条曲线：用包内最大轴位移近似
 * 速度，低速 1:1，随速度线性升到 1.75 倍。增益以 1/16 像素累加，
 * 小位移不会被取整吞掉，闭环瞄准仍能精确收敛。 */
#define PS2_ACCEL_FLOOR 3   /* <=3px：1:1，精细瞄准 */
#define PS2_ACCEL_CEIL 20   /* >=20px：达到最大增益 */
#define PS2_ACCEL_MAX16 28  /* 28/16 = 1.75 倍 */
static int64_t pointer_acc_x,pointer_acc_y;

static void ps2_flush(void){
 for(unsigned i=0;i<64;i++){
  uint8_t status=inb(PS2_COMMAND);
  if(!(status&PS2_STATUS_OBF))break;
  inb(PS2_DATA);
 }
}
static void ps2_wait_write(void){
 for(unsigned i=0;i<200000;i++)if(!(inb(PS2_COMMAND)&PS2_STATUS_IBF))return;
}
static int ps2_aux_command(uint8_t command){
 ps2_wait_write();outb(PS2_COMMAND,0xD4);
 ps2_wait_write();outb(PS2_DATA,command);
 /* 固件可能留下迟到的字节：只认 0xFA/0xFE，其余继续排空。 */
 for(unsigned i=0;i<200000;i++){
  uint8_t status=inb(PS2_COMMAND);
  if(!(status&PS2_STATUS_OBF))continue;
  uint8_t data=inb(PS2_DATA);
  if(data==0xFA||data==0xFE)return data==0xFA;
 }
 return 0;
}
/* 自适应增益（1/16 为单位）：低速 1:1，快速线性放大到 1.75 倍。 */
static int32_t ps2_gain16(int32_t dx,int32_t dy){
 int32_t ax=dx<0?-dx:dx,ay=dy<0?-dy:dy,peak=ax>ay?ax:ay;
 if(peak<=PS2_ACCEL_FLOOR)return 16;
 if(peak>=PS2_ACCEL_CEIL)return PS2_ACCEL_MAX16;
 return 16+((peak-PS2_ACCEL_FLOOR)*(PS2_ACCEL_MAX16-16))/(PS2_ACCEL_CEIL-PS2_ACCEL_FLOOR);
}
static void ring_push(uint32_t state){
 if(ring_count==PS2_RING){ring_tail=(ring_tail+1)%PS2_RING;ring_count--;}
 ring_states[ring_head]=state;ring_head=(ring_head+1)%PS2_RING;ring_count++;
}
static int ring_pop(uint32_t*out){
 if(!ring_count)return 0;
 *out=ring_states[ring_tail];ring_tail=(ring_tail+1)%PS2_RING;ring_count--;
 return 1;
}
/* 完成一个 3 字节包：更新坐标并记录快照。 */
static void ps2_commit(void){
 int32_t dx=(int32_t)packet[1];
 int32_t dy=(int32_t)packet[2];
 if(packet[0]&0x10)dx-=256;
 if(packet[0]&0x20)dy-=256;
 pointer_buttons=packet[0]&7u;
 /* 先按自适应增益累加（1/16 像素），再取整写入坐标，余量留到下一包。 */
 int32_t gain=ps2_gain16(dx,dy);
 pointer_acc_x+=(int64_t)dx*gain;
 pointer_acc_y+=(int64_t)dy*gain;
 int32_t mx=(int32_t)(pointer_acc_x/16),my=(int32_t)(pointer_acc_y/16);
 pointer_acc_x-=mx*16;pointer_acc_y-=my*16;
 pointer_x+=mx;
 pointer_y-=my;
 int32_t cx=pointer_x,cy=pointer_y;
 if(pointer_x<0)pointer_x=0;
 if(pointer_y<0)pointer_y=0;
 if(pointer_width&&pointer_x>(int32_t)pointer_width-1)pointer_x=(int32_t)pointer_width-1;
 if(pointer_height&&pointer_y>(int32_t)pointer_height-1)pointer_y=(int32_t)pointer_height-1;
 /* 撞到边界时清掉亚像素余量，避免回头时先补一大段旧位移。 */
 if(pointer_x!=cx||pointer_y!=cy)pointer_acc_x=pointer_acc_y=0;
 ring_push(0x80000000u|((uint32_t)pointer_buttons<<26)|
           ((uint32_t)pointer_y<<13)|(uint32_t)pointer_x);
}
/* 排空 i8042 输出缓冲：IRQ12 与轮询兜底共用。非 AUX 字节（键盘）直接丢弃，
 * 但必须读走，否则共享输出缓冲会一直保持 OBF。 */
static int ps2_drain(void){
 int any=0;
 for(unsigned n=0;n<128;n++){
  uint8_t status=inb(PS2_COMMAND);
  if(!(status&PS2_STATUS_OBF))break;
  uint8_t byte=inb(PS2_DATA);
  if(!(status&PS2_STATUS_AUX))continue;
  if(packet_index==0&&!(byte&0x08))continue;
  packet[packet_index++]=byte;
  if(packet_index<3)continue;
  packet_index=0;
  ps2_commit();
  any=1;
 }
 return any;
}
/* IRQ12 或共享 PCI 向量里发现 i8042 有待处理数据时调用（中断上下文，
 * 已持内核锁）。 */
void ps2_irq(void){
 if(!pointer_ready)return;
 if(!irq_logged){irq_logged=1;serial("[PS2] irq12 active\n");}
 ps2_drain();
}
/* 打开 i8042 的 AUX 中断，并在 8259 上放开级联 IRQ2 与 IRQ12。 */
static void ps2_enable_aux_irq(void){
 ps2_wait_write();outb(PS2_COMMAND,0x20);
 uint8_t config=0;
 for(unsigned i=0;i<200000;i++){
  uint8_t status=inb(PS2_COMMAND);
  if(status&PS2_STATUS_OBF){config=inb(PS2_DATA);break;}
 }
 ps2_wait_write();outb(PS2_COMMAND,0x60);
 ps2_wait_write();outb(PS2_DATA,(uint8_t)(config|0x02));
 outb(0x21,(uint8_t)(inb(0x21)&~0x04u)); /* 主片 IRQ2：从片级联 */
 outb(0xa1,(uint8_t)(inb(0xa1)&~0x10u)); /* 从片 IRQ12：PS/2 鼠标 */
}
void ps2_mouse_init(uint32_t width,uint32_t height){
 pointer_width=width;pointer_height=height;
 pointer_x=(int32_t)(width/2u);
 pointer_y=(int32_t)(height/2u);
 pointer_buttons=0;packet_index=0;
 pointer_acc_x=pointer_acc_y=0;
 ring_head=ring_tail=ring_count=0;
 ps2_wait_write();outb(PS2_COMMAND,0xA8);
 ps2_flush();
 /* 启用数据上报；ACK 偶尔会因固件残留字节而丢失，因此无论是否收到
  * ACK 都继续——没有事件时 ps2_mouse_read 返回 WOULD_BLOCK。 */
 for(unsigned attempt=0;attempt<3;attempt++)if(ps2_aux_command(0xF4))break;
 ps2_flush();
 /* 中断配置最后做：此后数据到达即进环，不再依赖 PID1 的轮询时机。 */
 ps2_enable_aux_irq();
 pointer_ready=1;
}
long ps2_mouse_read(int reset){
 if(!pointer_ready)return SF_WOULD_BLOCK;
 if(reset){
  pointer_x=(int32_t)(pointer_width/2u);
  pointer_y=(int32_t)(pointer_height/2u);
  pointer_buttons=0;packet_index=0;
  pointer_acc_x=pointer_acc_y=0;
  ring_head=ring_tail=ring_count=0;
  ps2_flush();
  return SF_WOULD_BLOCK;
 }
 /* IRQ12 没接通时（固件/平台差异）仍按原方式轮询兜底。 */
 ps2_drain();
 uint32_t state;
 if(!ring_pop(&state))return SF_WOULD_BLOCK;
 return (long)state;
}
