/* Legacy virtio-net in ring 3: two polling queues, bounded ARP, ICMP, UDP and
 * one-shot TCP exchanges. No interrupt routing; the kernel owns device
 * authorization and the DMA arena.
 */
#include "libsf.h"
#include "net.h"
#include "virtio.h"
#define RX_BUFFERS 16u
#define NET_HDR 10u /* Legacy virtio_net_hdr precedes every frame. */
static const SfBootArgs*boot;
static uint8_t our_mac[6],target_mac[6];
static uint16_t qsize[2],q_avail_idx[2],q_used_idx[2];
static uint32_t q_desc[2],q_avail[2],q_used[2];
static uint32_t rx_offset,tx_offset;
static int online;
static uint32_t rx_frames,tx_frames;
static uint8_t incoming[SF_NET_FRAME_MAX];
static void barrier(void){__atomic_thread_fence(__ATOMIC_SEQ_CST);}
static uint32_t round_page(uint32_t value){return(value+4095u)&~4095u;}
static long io(unsigned offset,unsigned width,int write,uint32_t value){
 if(offset>64u-width||(width!=1&&width!=2&&width!=4))return SF_INVALID;
 return sf_sys(SYS_PORT_IO,boot->device_io+offset,width|(write?256u:0),value);
}
static int reset(void){online=0;return(int)sf_sys(SYS_DEVICE_RELEASE,0,0,0);}
static int setup_queue(unsigned index,uint32_t base){
 if(io(14,2,1,index))return SF_DEVICE_GONE;
 long size=io(12,2,0,0);if(size<3||size>256||(size&(size-1)))return SF_BAD_FORMAT;
 uint32_t used=(16u*(uint32_t)size+6u+2u*(uint32_t)size+4095u)&~4095u;
 if(base>SF_DMA_BYTES||used+6u+8u*(uint32_t)size>SF_DMA_BYTES-base)return SF_BAD_FORMAT;
 qsize[index]=(uint16_t)size;q_desc[index]=base;q_avail[index]=base+16u*(uint32_t)size;q_used[index]=base+used;
 memset((void*)(SF_DMA+base),0,used+6u+8u*(uint32_t)size);
 *(volatile uint16_t*)(SF_DMA+q_avail[index])=1;q_avail_idx[index]=0;q_used_idx[index]=0;return 0;
}
static int start(void){
 if(!boot->device_io||boot->dma_virtual!=SF_DMA||!boot->dma_physical||(boot->dma_physical&4095))return SF_DENIED;
 if(boot->device_rights&SF_DEVICE_MODERN)return SF_DENIED;
 if(io(18,1,1,0)||io(18,1,0,0)!=0)return SF_DEVICE_GONE;
 if(io(18,1,1,1)||io(18,1,1,3))return SF_DEVICE_GONE;
 long features=io(0,4,0,0);if(features<0)return(int)features;
 if(!(features&(1u<<5)))return SF_DENIED;
 if(io(4,4,1,1u<<5))return SF_DEVICE_GONE;
 for(unsigned i=0;i<6;i++){long v=io(0x14+i,1,0,0);if(v<0)return(int)v;our_mac[i]=(uint8_t)v;}
 int r=setup_queue(0,0);if(r)return r;
 if(io(8,4,1,(uint32_t)(boot->dma_physical>>12)))return SF_DEVICE_GONE;
 uint32_t q1base=round_page(q_used[0]+6u+8u*qsize[0]);
 r=setup_queue(1,q1base);if(r)return r;
 if(io(8,4,1,(uint32_t)((boot->dma_physical+q1base)>>12)))return SF_DEVICE_GONE;
 rx_offset=round_page(q_used[1]+6u+8u*qsize[1]);
 uint32_t end=rx_offset+RX_BUFFERS*SF_NET_FRAME_MAX;
 tx_offset=round_page(end);
 if(tx_offset+SF_NET_FRAME_MAX>SF_DMA_BYTES)return SF_NO_SPACE;
 SfVirtqDescriptor*d=(void*)(SF_DMA+q_desc[0]);
 for(unsigned i=0;i<RX_BUFFERS;i++)d[i]=(SfVirtqDescriptor){boot->dma_physical+rx_offset+i*SF_NET_FRAME_MAX,SF_NET_FRAME_MAX,2,0};
 volatile uint16_t*avail=(void*)(SF_DMA+q_avail[0]);
 for(unsigned i=0;i<RX_BUFFERS;i++)avail[2+i]=(uint16_t)i;
 q_avail_idx[0]=RX_BUFFERS;barrier();avail[1]=RX_BUFFERS;barrier();
 if(io(16,2,1,0))return SF_DEVICE_GONE;
 barrier();if(io(18,1,1,7)||io(18,1,0,0)!=7)return SF_DEVICE_GONE;
 online=1;return 0;
}
static int receive(unsigned*length){
 if(!online)return SF_DEVICE_GONE;
 volatile uint16_t*used=(void*)(SF_DMA+q_used[0]);
 if(used[1]==q_used_idx[0])return SF_WOULD_BLOCK;
 uint16_t slot=q_used_idx[0]%qsize[0];
 volatile uint32_t*entry=(void*)(SF_DMA+q_used[0]+4u+8u*(uint32_t)slot);
 uint32_t id=entry[0],bytes=entry[1];
 if(id>=RX_BUFFERS||bytes>SF_NET_FRAME_MAX||bytes<NET_HDR){online=0;return SF_BAD_FORMAT;}
 uint32_t payload=bytes-NET_HDR;
 memcpy(incoming,(const void*)(SF_DMA+rx_offset+id*SF_NET_FRAME_MAX+NET_HDR),payload);
 SfVirtqDescriptor*d=(void*)(SF_DMA+q_desc[0]);
 d[id]=(SfVirtqDescriptor){boot->dma_physical+rx_offset+id*SF_NET_FRAME_MAX,SF_NET_FRAME_MAX,2,0};
 volatile uint16_t*avail=(void*)(SF_DMA+q_avail[0]);
 avail[2+(q_avail_idx[0]%qsize[0])]=(uint16_t)id;barrier();q_avail_idx[0]++;avail[1]=q_avail_idx[0];barrier();
 q_used_idx[0]++;rx_frames++;
 if(io(16,2,1,0)){online=0;return SF_DEVICE_GONE;}
 *length=payload;return 0;
}
static int transmit(const uint8_t*frame,unsigned bytes){
 if(!online)return SF_DEVICE_GONE;if(bytes>SF_NET_FRAME_MAX-NET_HDR)return SF_INVALID;
 uint8_t*buffer=(void*)(SF_DMA+tx_offset);memset(buffer,0,NET_HDR);memcpy(buffer+NET_HDR,frame,bytes);
 SfVirtqDescriptor*d=(void*)(SF_DMA+q_desc[1]);
 d[0]=(SfVirtqDescriptor){boot->dma_physical+tx_offset,bytes+NET_HDR,0,0};barrier();
 volatile uint16_t*avail=(void*)(SF_DMA+q_avail[1]);
 avail[2+(q_avail_idx[1]%qsize[1])]=0;barrier();q_avail_idx[1]++;avail[1]=q_avail_idx[1];barrier();
 if(io(16,2,1,1)){online=0;return SF_DEVICE_GONE;}
 volatile uint16_t*used=(void*)(SF_DMA+q_used[1]);
 uint64_t deadline=sf_ticks()+10;
 while(used[1]==q_used_idx[1]){if(sf_ticks()>=deadline){online=0;return SF_TIMEOUT;}sf_yield();}
 q_used_idx[1]=used[1];tx_frames++;return 0;
}
static int wait_arp(uint32_t ip,unsigned timeout){
 uint64_t deadline=sf_ticks()+timeout;
 for(;;){unsigned bytes;int r=receive(&bytes);
  if(!r){if(sf_net_parse_arp_reply(incoming,bytes,ip,target_mac))return 0;}
  else if(r!=SF_WOULD_BLOCK)return r;
  if(sf_ticks()>=deadline)return SF_TIMEOUT;sf_yield();}
}
static int wait_icmp(uint32_t ip,uint16_t seq,unsigned timeout){
 uint64_t deadline=sf_ticks()+timeout;
 for(;;){unsigned bytes;int r=receive(&bytes);
  if(!r){if(sf_net_parse_icmp_reply(incoming,bytes,ip,seq))return 0;}
  else if(r!=SF_WOULD_BLOCK)return r;
  if(sf_ticks()>=deadline)return SF_TIMEOUT;sf_yield();}
}
static int ping(uint32_t ip,uint64_t*elapsed){
 uint64_t started=sf_ticks();uint8_t frame[58];unsigned bytes;int r=SF_TIMEOUT;
 for(unsigned attempt=0;attempt<3&&r!=SF_DEVICE_GONE;attempt++){
  bytes=sf_net_build_arp_request(frame,our_mac,SF_NET_IPV4_LEN,ip);r=transmit(frame,bytes);if(r)break;
  r=wait_arp(ip,20);if(!r)break;if(r!=SF_TIMEOUT)break;
 }
 if(r)return r;
 for(unsigned attempt=0;attempt<3;attempt++){
  uint16_t seq=(uint16_t)(attempt+1);bytes=sf_net_build_icmp_echo(frame,our_mac,target_mac,SF_NET_IPV4_LEN,ip,seq);
  r=transmit(frame,bytes);if(r)break;r=wait_icmp(ip,seq,20);if(!r)break;if(r!=SF_TIMEOUT)break;
 }
 if(!r&&elapsed)*elapsed=sf_ticks()-started;return r;
}
static int wait_udp(uint32_t ip,uint16_t port,uint8_t*payload,unsigned*payload_bytes,unsigned timeout){
 uint64_t deadline=sf_ticks()+timeout;
 for(;;){unsigned bytes;int r=receive(&bytes);
  if(!r){unsigned count=0;if(sf_net_parse_udp(incoming,bytes,ip,port,SF_NET_UDP_PORT,payload,&count)){*payload_bytes=count;return 0;}}
  else if(r!=SF_WOULD_BLOCK)return r;
  if(sf_ticks()>=deadline)return SF_TIMEOUT;sf_yield();}
}
/* One-shot bounded datagram exchange: resolve, send, wait for the peer reply. */
static int udp_exchange(uint32_t ip,uint16_t port,const uint8_t*payload,unsigned bytes,uint8_t*reply,unsigned*reply_bytes,uint64_t*elapsed){
 uint64_t started=sf_ticks();uint8_t frame[SF_NET_FRAME_MAX];unsigned total;int r=SF_TIMEOUT;
 for(unsigned attempt=0;attempt<3&&r!=SF_DEVICE_GONE;attempt++){
  total=sf_net_build_arp_request(frame,our_mac,SF_NET_IPV4_LEN,ip);r=transmit(frame,total);if(r)break;
  r=wait_arp(ip,20);if(!r)break;if(r!=SF_TIMEOUT)break;
 }
 if(r)return r;
 total=sf_net_build_udp(frame,our_mac,target_mac,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,ip,port,payload,bytes);
 if(!total)return SF_INVALID;
 r=transmit(frame,total);if(r)return r;
 r=wait_udp(ip,port,reply,reply_bytes,100);
 if(!r&&elapsed)*elapsed=sf_ticks()-started;return r;
}
/* Accept one TCP segment from the peer; mask 0 accepts any flags. */
static int wait_tcp(uint32_t ip,uint16_t port,uint8_t flags,uint8_t mask,uint32_t*seq,uint32_t*ack,uint8_t*got,uint8_t*payload,unsigned*payload_bytes,unsigned timeout){
 uint64_t deadline=sf_ticks()+timeout;
 for(;;){unsigned bytes;int r=receive(&bytes);
  if(!r){uint32_t s=0,a=0;uint8_t f=0;uint16_t w=0;unsigned count=0;
   if(sf_net_parse_tcp(incoming,bytes,ip,port,SF_NET_TCP_PORT,&s,&a,&f,&w,payload,&count)&&(f&mask)==flags){
    if(seq)*seq=s;if(ack)*ack=a;if(got)*got=f;if(payload_bytes)*payload_bytes=count;return 0;}}
  else if(r!=SF_WOULD_BLOCK)return r;
  if(sf_ticks()>=deadline)return SF_TIMEOUT;sf_yield();}
}
/* One-shot bounded TCP exchange: handshake, one data segment each way, close. */
static int tcp_exchange(uint32_t ip,uint16_t port,const uint8_t*payload,unsigned bytes,uint8_t*reply,unsigned*reply_bytes,uint64_t*elapsed){
 uint64_t started=sf_ticks();uint8_t frame[SF_NET_FRAME_MAX];unsigned total;int r=SF_TIMEOUT;
 for(unsigned attempt=0;attempt<3&&r!=SF_DEVICE_GONE;attempt++){
  total=sf_net_build_arp_request(frame,our_mac,SF_NET_IPV4_LEN,ip);r=transmit(frame,total);if(r)break;
  r=wait_arp(ip,20);if(!r)break;if(r!=SF_TIMEOUT)break;
 }
 if(r)return r;
 uint32_t seq=0x53460001u,peer_seq=0,ack=0;uint8_t flags=0;unsigned count=0;
 for(unsigned attempt=0;attempt<3;attempt++){
  total=sf_net_build_tcp(frame,our_mac,target_mac,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,ip,port,seq,0,SF_NET_TCP_SYN,SF_NET_TCP_WINDOW,0,0);
  if(!total)return SF_INVALID;
  r=transmit(frame,total);if(r)return r;
  r=wait_tcp(ip,port,SF_NET_TCP_SYN|SF_NET_TCP_ACK,SF_NET_TCP_SYN|SF_NET_TCP_ACK,&peer_seq,&ack,&flags,reply,&count,20);
  if(!r&&ack!=seq+1)r=SF_TIMEOUT;
  if(!r||r!=SF_TIMEOUT)break;
 }
 if(r)return r;
 total=sf_net_build_tcp(frame,our_mac,target_mac,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,ip,port,seq+1,peer_seq+1,SF_NET_TCP_ACK,SF_NET_TCP_WINDOW,0,0);
 if(!total)return SF_INVALID;
 r=transmit(frame,total);if(r)return r;
 uint32_t sent=seq+1;
 total=sf_net_build_tcp(frame,our_mac,target_mac,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,ip,port,sent,peer_seq+1,SF_NET_TCP_PSH|SF_NET_TCP_ACK,SF_NET_TCP_WINDOW,payload,bytes);
 if(!total)return SF_INVALID;
 r=transmit(frame,total);if(r)return r;
 sent+=(uint32_t)bytes;
 for(unsigned attempt=0;attempt<8;attempt++){
  r=wait_tcp(ip,port,0,0,&peer_seq,&ack,&flags,reply,&count,40);
  if(!r){if(count)break;if(flags&(SF_NET_TCP_RST|SF_NET_TCP_FIN)){r=SF_TIMEOUT;break;}continue;}
  if(r!=SF_TIMEOUT)break;
  if(attempt<2){r=transmit(frame,total);if(r)break;}
 }
 if(!r&&!count)r=SF_TIMEOUT;
 if(r)return r;
 *reply_bytes=count;
 total=sf_net_build_tcp(frame,our_mac,target_mac,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,ip,port,sent,peer_seq+(uint32_t)count,SF_NET_TCP_ACK,SF_NET_TCP_WINDOW,0,0);
 if(total)transmit(frame,total);
 total=sf_net_build_tcp(frame,our_mac,target_mac,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,ip,port,sent,peer_seq+(uint32_t)count,SF_NET_TCP_FIN|SF_NET_TCP_ACK,SF_NET_TCP_WINDOW,0,0);
 if(total)transmit(frame,total);
 if(elapsed)*elapsed=sf_ticks()-started;return 0;
}
long sf_main(const SfBootArgs*a,size_t size){
 boot=a;if(size!=sizeof(*a)||a->abi_major!=1)return SF_ABI_MISMATCH;
 int startup=start();if(startup)reset();
 sf_log(startup?"[NET] device initialization refused\n":"[NET] virtio-net service in ring 3\n");
 for(;;){SfMessage m;long sender=sf_recv(a->service,&m,0);if(sender<0){reset();return sender;}int result=0;
  if(sender!=1)result=SF_DENIED;
  else if(startup)result=startup;
  else if(!online)result=SF_DEVICE_GONE;
  else if(m.h.opcode==OP_NET_INFO){SfNetInfo info;memset(&info,0,sizeof(info));memcpy(info.mac,our_mac,6);info.ip=SF_NET_IPV4_LEN;info.gateway=SF_NET_GATEWAY;info.netmask=SF_NET_NETMASK;info.rx_frames=rx_frames;info.tx_frames=tx_frames;info.link=1;m.length=sizeof(info);memcpy(m.data,&info,sizeof(info));}
  else if(m.h.opcode==OP_NET_PING){uint32_t ip=0;uint64_t elapsed=0;
   if(m.length<1||m.length>64||m.data[m.length-1])result=SF_INVALID;
   else result=sf_net_parse_ipv4((const char*)m.data,&ip);
   if(!result){result=ping(ip,&elapsed);m.mode=(uint32_t)elapsed;}}
  else if(m.h.opcode==OP_NET_UDP){SfNetPacket packet;uint64_t elapsed=0;unsigned count=0;uint8_t reply[SF_NET_PAYLOAD_MAX];uint32_t peer_ip=0;uint16_t peer_port=0;
   if(m.length!=sizeof(packet))result=SF_BAD_FORMAT;
   else {memcpy(&packet,m.data,sizeof(packet));peer_ip=packet.ip;peer_port=packet.port;
    if(packet.bytes>SF_NET_PAYLOAD_MAX||!packet.port)result=SF_INVALID;
    else result=udp_exchange(packet.ip,packet.port,packet.data,packet.bytes,reply,&count,&elapsed);
    if(!result){memset(&packet,0,sizeof(packet));packet.ip=peer_ip;packet.port=peer_port;packet.bytes=(uint16_t)count;memcpy(packet.data,reply,count);m.length=sizeof(packet);memcpy(m.data,&packet,sizeof(packet));m.mode=(uint32_t)elapsed;}}}
  else if(m.h.opcode==OP_NET_TCP){SfNetPacket packet;uint64_t elapsed=0;unsigned count=0;uint8_t reply[SF_NET_PAYLOAD_MAX];uint32_t peer_ip=0;uint16_t peer_port=0;
   if(m.length!=sizeof(packet))result=SF_BAD_FORMAT;
   else {memcpy(&packet,m.data,sizeof(packet));peer_ip=packet.ip;peer_port=packet.port;
    if(packet.bytes>SF_NET_PAYLOAD_MAX||!packet.port)result=SF_INVALID;
    else result=tcp_exchange(packet.ip,packet.port,packet.data,packet.bytes,reply,&count,&elapsed);
    if(!result){memset(&packet,0,sizeof(packet));packet.ip=peer_ip;packet.port=peer_port;packet.bytes=(uint16_t)count;memcpy(packet.data,reply,count);m.length=sizeof(packet);memcpy(m.data,&packet,sizeof(packet));m.mode=(uint32_t)elapsed;}}}
  else result=SF_INVALID;
  m.status=result;if(result)m.length=0;sf_send(a->manager,&m);
 }
}
