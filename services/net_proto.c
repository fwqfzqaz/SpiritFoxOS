/* Deterministic Ethernet/ARP/IPv4 framing without syscalls or device state. */
#include "net.h"
static void put16(uint8_t*p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void put32(uint8_t*p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static uint16_t get16(const uint8_t*p){return(uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static uint32_t get32(const uint8_t*p){return((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
uint16_t sf_net_checksum(uint32_t sum,const void*data,unsigned bytes){
 const uint8_t*p=data;while(bytes>1){sum+=((uint32_t)p[0]<<8)|p[1];p+=2;bytes-=2;}
 if(bytes)sum+=(uint32_t)p[0]<<8;while(sum>>16)sum=(sum&0xffffu)+(sum>>16);return(uint16_t)(~sum&0xffffu);
}
int sf_net_parse_ipv4(const char*text,uint32_t*out){
 if(!text||!out)return SF_INVALID;uint32_t ip=0;unsigned part=0;
 while(part<4){if(*text<'0'||*text>'9')return SF_INVALID;unsigned value=0,digits=0;
  while(*text>='0'&&*text<='9'){if(digits==3)return SF_INVALID;value=value*10u+(unsigned)(*text-'0');if(value>255)return SF_INVALID;digits++;text++;}
  if(!digits)return SF_INVALID;ip=(ip<<8)|value;part++;
  if(part<4){if(*text!='.')return SF_INVALID;text++;}
 }
 if(*text)return SF_INVALID;*out=ip;return SF_OK;
}
void sf_net_format_ipv4(uint32_t ip,char out[16]){
 static const uint32_t shifts[4]={24,16,8,0};unsigned n=0;
 for(unsigned i=0;i<4;i++){unsigned value=(ip>>shifts[i])&255u,digits=0;char reversed[3];
  do{reversed[digits++]=(char)('0'+value%10u);value/=10u;}while(value);
  while(digits)out[n++]=reversed[--digits];if(i!=3)out[n++]='.';}
 out[n]=0;
}
unsigned sf_net_build_arp_request(uint8_t frame[42],const uint8_t mac[6],uint32_t src,uint32_t dst){
 memset(frame,0,42);for(unsigned i=0;i<6;i++)frame[i]=0xff;memcpy(frame+6,mac,6);put16(frame+12,SF_NET_ARP);
 uint8_t*a=frame+14;put16(a,1);put16(a+2,SF_NET_IPV4);a[4]=6;a[5]=4;put16(a+6,1);memcpy(a+8,mac,6);
 put32(a+14,src);put32(a+24,dst);return 42;
}
unsigned sf_net_build_icmp_echo(uint8_t frame[58],const uint8_t src_mac[6],const uint8_t dst_mac[6],uint32_t src,uint32_t dst,uint16_t seq){
 memset(frame,0,58);memcpy(frame,dst_mac,6);memcpy(frame+6,src_mac,6);put16(frame+12,SF_NET_IPV4);
 uint8_t*ip=frame+14;ip[0]=0x45;put16(ip+2,44);put16(ip+4,1);put16(ip+6,0x4000);ip[8]=64;ip[9]=1;
 put32(ip+12,src);put32(ip+16,dst);put16(ip+10,sf_net_checksum(0,ip,20));
 uint8_t*icmp=ip+20;icmp[0]=8;put16(icmp+4,SF_NET_ICMP_ID);put16(icmp+6,seq);
 for(unsigned i=0;i<16;i++)icmp[8+i]=(uint8_t)(0x40+i);
 put16(icmp+2,sf_net_checksum(0,icmp,24));return 58;
}
int sf_net_parse_arp_reply(const uint8_t*frame,unsigned bytes,uint32_t expect_ip,uint8_t mac_out[6]){
 if(bytes<42||get16(frame+12)!=SF_NET_ARP)return 0;const uint8_t*a=frame+14;
 if(get16(a)!=1||get16(a+2)!=SF_NET_IPV4||a[4]!=6||a[5]!=4||get16(a+6)!=2)return 0;
 if(get32(a+14)!=expect_ip)return 0;if(mac_out)memcpy(mac_out,a+8,6);return 1;
}
int sf_net_parse_icmp_reply(const uint8_t*frame,unsigned bytes,uint32_t expect_src,uint16_t seq){
 if(bytes<42||get16(frame+12)!=SF_NET_IPV4)return 0;const uint8_t*ip=frame+14;
 unsigned ihl=ip[0]&15u;if((ip[0]>>4)!=4||ihl<5||14+ihl*4+8>bytes)return 0;
 uint16_t total=get16(ip+2);if(total<ihl*4+8||total>bytes-14)return 0;
 if((get16(ip+6)&0x3fffu)||ip[9]!=1||get32(ip+12)!=expect_src||sf_net_checksum(0,ip,ihl*4))return 0;
 const uint8_t*icmp=ip+ihl*4;if(icmp[0]!=0||icmp[1]||get16(icmp+4)!=SF_NET_ICMP_ID||get16(icmp+6)!=seq)return 0;
 return 1;
}
/* Raw one's-complement accumulator; sf_net_checksum folds and complements it. */
static uint32_t add_words(uint32_t sum,const uint8_t*p,unsigned bytes){
 while(bytes>1){sum+=((uint32_t)p[0]<<8)|p[1];p+=2;bytes-=2;}
 if(bytes)sum+=(uint32_t)p[0]<<8;return sum;
}
static uint16_t fold(uint32_t sum){while(sum>>16)sum=(sum&0xffffu)+(sum>>16);return(uint16_t)(~sum&0xffffu);}
static uint16_t pseudo_checksum(uint8_t proto,uint32_t src,uint32_t dst,const void*segment,unsigned bytes){
 uint8_t pseudo[12];put32(pseudo,src);put32(pseudo+4,dst);pseudo[8]=0;pseudo[9]=proto;put16(pseudo+10,(uint16_t)bytes);
 uint32_t sum=add_words(add_words(0,pseudo,12),segment,bytes);return fold(sum);
}
uint16_t sf_net_udp_checksum(uint32_t src,uint32_t dst,const void*segment,unsigned bytes){
 return pseudo_checksum(SF_NET_UDP,src,dst,segment,bytes);
}
uint16_t sf_net_tcp_checksum(uint32_t src,uint32_t dst,const void*segment,unsigned bytes){
 return pseudo_checksum(SF_NET_TCP,src,dst,segment,bytes);
}
unsigned sf_net_build_udp(uint8_t frame[SF_NET_FRAME_MAX],const uint8_t src_mac[6],const uint8_t dst_mac[6],uint32_t src,uint16_t sport,uint32_t dst,uint16_t dport,const void*payload,unsigned bytes){
 if(bytes>SF_NET_PAYLOAD_MAX||bytes>SF_NET_FRAME_MAX-(14u+20u+8u))return 0;
 memset(frame,0,14u+20u+8u+bytes);memcpy(frame,dst_mac,6);memcpy(frame+6,src_mac,6);put16(frame+12,SF_NET_IPV4);
 uint8_t*ip=frame+14;ip[0]=0x45;put16(ip+2,(uint16_t)(28u+bytes));put16(ip+4,1);put16(ip+6,0x4000);ip[8]=64;ip[9]=SF_NET_UDP;
 put32(ip+12,src);put32(ip+16,dst);put16(ip+10,sf_net_checksum(0,ip,20));
 uint8_t*udp=ip+20;put16(udp,(uint16_t)sport);put16(udp+2,(uint16_t)dport);put16(udp+4,(uint16_t)(8u+bytes));
 if(bytes&&payload)memcpy(udp+8,payload,bytes);
 uint16_t checksum=sf_net_udp_checksum(src,dst,udp,8u+bytes);put16(udp+6,checksum?checksum:0xffffu);
 return 14u+20u+8u+bytes;
}
int sf_net_parse_udp(const uint8_t*frame,unsigned bytes,uint32_t expect_src,uint16_t expect_sport,uint16_t expect_dport,uint8_t payload[SF_NET_PAYLOAD_MAX],unsigned*payload_bytes){
 if(bytes<42||get16(frame+12)!=SF_NET_IPV4)return 0;const uint8_t*ip=frame+14;
 unsigned ihl=ip[0]&15u;if((ip[0]>>4)!=4||ihl<5||14+ihl*4+8>bytes)return 0;
 uint16_t total=get16(ip+2);if(total<ihl*4+8||total>bytes-14)return 0;
 if((get16(ip+6)&0x3fffu)||ip[9]!=SF_NET_UDP||get32(ip+12)!=expect_src||sf_net_checksum(0,ip,ihl*4))return 0;
 const uint8_t*udp=ip+ihl*4;uint16_t length=get16(udp+4);
 if(length<8||length>total-ihl*4)return 0;unsigned count=length-8u;if(count>SF_NET_PAYLOAD_MAX)return 0;
 if(get16(udp)!=expect_sport||get16(udp+2)!=expect_dport)return 0;
 uint16_t checksum=get16(udp+6);
 /* IPv4 permits an absent UDP checksum; a present one must verify. */
 if(checksum&&sf_net_udp_checksum(get32(ip+12),get32(ip+16),udp,length))return 0;
 if(count)memcpy(payload,udp+8,count);if(payload_bytes)*payload_bytes=count;return 1;
}
unsigned sf_net_build_tcp(uint8_t frame[SF_NET_FRAME_MAX],const uint8_t src_mac[6],const uint8_t dst_mac[6],uint32_t src,uint16_t sport,uint32_t dst,uint16_t dport,uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void*payload,unsigned bytes){
 if(bytes>SF_NET_PAYLOAD_MAX||bytes>SF_NET_FRAME_MAX-(14u+20u+20u))return 0;
 memset(frame,0,14u+20u+20u+bytes);memcpy(frame,dst_mac,6);memcpy(frame+6,src_mac,6);put16(frame+12,SF_NET_IPV4);
 uint8_t*ip=frame+14;ip[0]=0x45;put16(ip+2,(uint16_t)(40u+bytes));put16(ip+4,1);put16(ip+6,0x4000);ip[8]=64;ip[9]=SF_NET_TCP;
 put32(ip+12,src);put32(ip+16,dst);put16(ip+10,sf_net_checksum(0,ip,20));
 uint8_t*tcp=ip+20;put16(tcp,(uint16_t)sport);put16(tcp+2,(uint16_t)dport);put32(tcp+4,seq);put32(tcp+8,ack);
 tcp[12]=0x50;tcp[13]=flags;put16(tcp+14,window);
 if(bytes&&payload)memcpy(tcp+20,payload,bytes);
 put16(tcp+16,sf_net_tcp_checksum(src,dst,tcp,20u+bytes));
 return 14u+20u+20u+bytes;
}
int sf_net_parse_tcp(const uint8_t*frame,unsigned bytes,uint32_t expect_src,uint16_t expect_sport,uint16_t expect_dport,uint32_t*seq,uint32_t*ack,uint8_t*flags,uint16_t*window,uint8_t payload[SF_NET_PAYLOAD_MAX],unsigned*payload_bytes){
 if(bytes<54||get16(frame+12)!=SF_NET_IPV4)return 0;const uint8_t*ip=frame+14;
 unsigned ihl=ip[0]&15u;if((ip[0]>>4)!=4||ihl<5||14+ihl*4+20>bytes)return 0;
 uint16_t total=get16(ip+2);if(total<ihl*4+20||total>bytes-14)return 0;
 if((get16(ip+6)&0x3fffu)||ip[9]!=SF_NET_TCP||get32(ip+12)!=expect_src||sf_net_checksum(0,ip,ihl*4))return 0;
 const uint8_t*tcp=ip+ihl*4;unsigned segment=total-ihl*4;
 unsigned offset=(tcp[12]>>4)*4;if(offset<20||ihl*4+offset>total)return 0;
 if(get16(tcp)!=expect_sport||get16(tcp+2)!=expect_dport)return 0;
 /* TCP checksums are mandatory; there is no "absent" encoding. */
 if(sf_net_tcp_checksum(get32(ip+12),get32(ip+16),tcp,segment))return 0;
 unsigned count=segment-offset;if(count>SF_NET_PAYLOAD_MAX)return 0;
 if(count)memcpy(payload,tcp+offset,count);
 if(seq)*seq=get32(tcp+4);if(ack)*ack=get32(tcp+8);if(flags)*flags=tcp[13];if(window)*window=get16(tcp+14);
 if(payload_bytes)*payload_bytes=count;return 1;
}
