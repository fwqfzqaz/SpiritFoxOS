#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"NET FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static uint16_t get16(const uint8_t*p){return(uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static uint32_t get32(const uint8_t*p){return((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void put16(uint8_t*p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void put32(uint8_t*p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static int checksum_valid(const uint8_t*p,unsigned n){return sf_net_checksum(0,p,n)==0;}
int main(void){
 uint8_t mac[6]={0x52,0x54,0x00,0x12,0x34,0x56},peer[6]={0x52,0x55,0x0a,0x00,0x02,0x02},got[6];
 uint8_t frame[58],reply[58];
 uint32_t ip=0;
 CHECK(!sf_net_parse_ipv4("10.0.2.2",&ip)&&ip==SF_NET_GATEWAY);
 CHECK(!sf_net_parse_ipv4("10.0.2.15",&ip)&&ip==SF_NET_IPV4_LEN);
 CHECK(!sf_net_parse_ipv4("0.0.0.0",&ip)&&ip==0);
 CHECK(!sf_net_parse_ipv4("255.255.255.255",&ip)&&ip==0xffffffffu);
 const char*bad[]={"","1","1.2","1.2.3","1.2.3.4.5","256.0.0.1","1..2.3","1.2.3.","1.2.3.4x","-1.2.3.4","1.2.3.256","9999.1.1.1","1.2.3.4 ",".1.2.3"};
 uint32_t before=ip;
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){CHECK(sf_net_parse_ipv4(bad[i],&ip)==SF_INVALID&&ip==before);}
 char text[16];sf_net_format_ipv4(SF_NET_IPV4_LEN,text);CHECK(!strcmp(text,"10.0.2.15"));
 sf_net_format_ipv4(0,text);CHECK(!strcmp(text,"0.0.0.0"));
 sf_net_format_ipv4(0xffffffffu,text);CHECK(!strcmp(text,"255.255.255.255"));
 sf_net_format_ipv4(SF_NET_GATEWAY,text);CHECK(!strcmp(text,"10.0.2.2"));
 unsigned n=sf_net_build_arp_request(frame,mac,SF_NET_IPV4_LEN,SF_NET_GATEWAY);
 CHECK(n==42);
 CHECK(!memcmp(frame,"\xff\xff\xff\xff\xff\xff",6)&&!memcmp(frame+6,mac,6)&&get16(frame+12)==SF_NET_ARP);
 CHECK(get16(frame+14)==1&&get16(frame+16)==SF_NET_IPV4&&frame[18]==6&&frame[19]==4&&get16(frame+20)==1);
 CHECK(!memcmp(frame+22,mac,6)&&get32(frame+28)==SF_NET_IPV4_LEN&&get32(frame+38)==SF_NET_GATEWAY);
 memcpy(reply,frame,42);reply[21]=2;memcpy(reply+22,peer,6);put32(reply+28,SF_NET_GATEWAY);memset(reply+32,0,6);
 CHECK(sf_net_parse_arp_reply(reply,42,SF_NET_GATEWAY,got)&&!memcmp(got,peer,6));
 CHECK(!sf_net_parse_arp_reply(reply,41,SF_NET_GATEWAY,got));
 CHECK(!sf_net_parse_arp_reply(reply,42,SF_NET_IPV4_LEN,got));
 reply[21]=1;CHECK(!sf_net_parse_arp_reply(reply,42,SF_NET_GATEWAY,got));
 reply[21]=2;put16(reply+12,SF_NET_IPV4);CHECK(!sf_net_parse_arp_reply(reply,42,SF_NET_GATEWAY,got));
 put16(reply+12,SF_NET_ARP);
 n=sf_net_build_icmp_echo(frame,mac,peer,SF_NET_IPV4_LEN,SF_NET_GATEWAY,7);
 CHECK(n==58);
 const uint8_t*ipf=frame+14;const uint8_t*icmp=ipf+20;
 CHECK(ipf[0]==0x45&&get16(ipf+2)==44&&ipf[8]==64&&ipf[9]==1&&get32(ipf+12)==SF_NET_IPV4_LEN&&get32(ipf+16)==SF_NET_GATEWAY);
 CHECK(checksum_valid(ipf,20)&&checksum_valid(icmp,24));
 CHECK(icmp[0]==8&&icmp[1]==0&&get16(icmp+4)==SF_NET_ICMP_ID&&get16(icmp+6)==7);
 CHECK(!memcmp(frame,peer,6)&&!memcmp(frame+6,mac,6)&&get16(frame+12)==SF_NET_IPV4);
 memcpy(reply,frame,58);uint8_t*rip=reply+14;uint8_t*ricmp=rip+20;
 put32(rip+12,SF_NET_GATEWAY);put32(rip+16,SF_NET_IPV4_LEN);put16(rip+10,0);put16(rip+10,sf_net_checksum(0,rip,20));
 ricmp[0]=0;put16(ricmp+2,0);put16(ricmp+2,sf_net_checksum(0,ricmp,24));
 CHECK(sf_net_parse_icmp_reply(reply,58,SF_NET_GATEWAY,7));
 CHECK(!sf_net_parse_icmp_reply(reply,58,SF_NET_GATEWAY,8));
 CHECK(!sf_net_parse_icmp_reply(reply,58,SF_NET_IPV4_LEN,7));
 CHECK(!sf_net_parse_icmp_reply(reply,57,SF_NET_GATEWAY,7));
 ricmp[0]=8;put16(ricmp+2,0);put16(ricmp+2,sf_net_checksum(0,ricmp,24));CHECK(!sf_net_parse_icmp_reply(reply,58,SF_NET_GATEWAY,7));
 ricmp[0]=0;put16(ricmp+2,0);put16(ricmp+2,sf_net_checksum(0,ricmp,24));
 rip[6]|=0x20;put16(rip+10,0);put16(rip+10,sf_net_checksum(0,rip,20));CHECK(!sf_net_parse_icmp_reply(reply,58,SF_NET_GATEWAY,7));
 rip[6]&=~0x20;put16(rip+10,0);put16(rip+10,sf_net_checksum(0,rip,20));
 put32(rip+12,0x01020304u);put16(rip+10,0);put16(rip+10,sf_net_checksum(0,rip,20));CHECK(!sf_net_parse_icmp_reply(reply,58,SF_NET_GATEWAY,7));
 uint32_t seed=20261030;
 for(unsigned i=0;i<1000;i++){
  seed=seed*1664525u+1013904223u;uint32_t target=seed;uint16_t seq=(uint16_t)(seed>>16);
  n=sf_net_build_icmp_echo(frame,mac,peer,SF_NET_IPV4_LEN,target,seq);CHECK(n==58);
  CHECK(checksum_valid(frame+14,20)&&checksum_valid(frame+34,24));
  memcpy(reply,frame,58);rip=reply+14;ricmp=rip+20;
  put32(rip+12,target);put32(rip+16,SF_NET_IPV4_LEN);put16(rip+10,0);put16(rip+10,sf_net_checksum(0,rip,20));
  ricmp[0]=0;put16(ricmp+2,0);put16(ricmp+2,sf_net_checksum(0,ricmp,24));
  CHECK(sf_net_parse_icmp_reply(reply,58,target,seq));
  CHECK(!sf_net_parse_icmp_reply(reply,58,target^1u,seq));
  CHECK(!sf_net_parse_icmp_reply(reply,58,target,(uint16_t)(seq+1)));
 }
 uint8_t packet[SF_NET_FRAME_MAX],back[SF_NET_FRAME_MAX],parsed[SF_NET_PAYLOAD_MAX],sent[SF_NET_PAYLOAD_MAX];
 unsigned count=0;
 const uint8_t text5[5]={1,2,3,4,5};
 n=sf_net_build_udp(packet,mac,peer,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,SF_NET_GATEWAY,9000,text5,5);
 CHECK(n==47);
 const uint8_t*uip=packet+14;const uint8_t*uudp=uip+20;
 CHECK(get16(packet+12)==SF_NET_IPV4&&!memcmp(packet,peer,6)&&!memcmp(packet+6,mac,6));
 CHECK(uip[0]==0x45&&get16(uip+2)==33&&uip[8]==64&&uip[9]==SF_NET_UDP&&get32(uip+12)==SF_NET_IPV4_LEN&&get32(uip+16)==SF_NET_GATEWAY);
 CHECK(checksum_valid(uip,20));
 CHECK(get16(uudp)==SF_NET_UDP_PORT&&get16(uudp+2)==9000&&get16(uudp+4)==13&&!memcmp(uudp+8,text5,5));
 CHECK(sf_net_parse_udp(packet,n,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,9000,parsed,&count)&&count==5&&!memcmp(parsed,text5,5));
 CHECK(!sf_net_parse_udp(packet,n,SF_NET_GATEWAY,SF_NET_UDP_PORT,9000,parsed,&count));
 CHECK(!sf_net_parse_udp(packet,n,SF_NET_IPV4_LEN,9999,9000,parsed,&count));
 CHECK(!sf_net_parse_udp(packet,n,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,9001,parsed,&count));
 CHECK(!sf_net_parse_udp(packet,n-1,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,9000,parsed,&count));
 CHECK(!sf_net_parse_udp(packet,n-5,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,9000,parsed,&count));
 n=sf_net_build_udp(back,peer,mac,SF_NET_GATEWAY,9000,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,text5,5);
 CHECK(n==47);
 CHECK(sf_net_parse_udp(back,n,SF_NET_GATEWAY,9000,SF_NET_UDP_PORT,parsed,&count)&&count==5&&!memcmp(parsed,text5,5));
 /* Absent IPv4 UDP checksum is legal; a present wrong one is rejected. */
 put16(back+40,0);CHECK(sf_net_parse_udp(back,n,SF_NET_GATEWAY,9000,SF_NET_UDP_PORT,parsed,&count));
 put16(back+40,0x1234);CHECK(!sf_net_parse_udp(back,n,SF_NET_GATEWAY,9000,SF_NET_UDP_PORT,parsed,&count));
 n=sf_net_build_udp(back,peer,mac,SF_NET_GATEWAY,9000,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,text5,5);
 put16(back+20,0x2000);put16(back+24,0);put16(back+24,sf_net_checksum(0,back+14,20));
 CHECK(!sf_net_parse_udp(back,n,SF_NET_GATEWAY,9000,SF_NET_UDP_PORT,parsed,&count));
 n=sf_net_build_udp(back,peer,mac,SF_NET_GATEWAY,9000,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,text5,5);
 back[23]=SF_NET_ICMP;put16(back+24,0);put16(back+24,sf_net_checksum(0,back+14,20));
 CHECK(!sf_net_parse_udp(back,n,SF_NET_GATEWAY,9000,SF_NET_UDP_PORT,parsed,&count));
 CHECK(sf_net_build_udp(back,peer,mac,SF_NET_GATEWAY,9000,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,sent,SF_NET_PAYLOAD_MAX+1)==0);
 uint32_t seed2=20261030u;
 for(unsigned i=0;i<1000;i++){
  seed2=seed2*1664525u+1013904223u;unsigned bytes=(unsigned)(seed2%249u);
  for(unsigned j=0;j<bytes;j++){seed2=seed2*1664525u+1013904223u;sent[j]=(uint8_t)(seed2>>24);}
  uint32_t target=0x0a000200u|(seed2&0xffu);uint16_t dport=(uint16_t)((seed2>>16)|1u);
  n=sf_net_build_udp(packet,mac,peer,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,target,dport,sent,bytes);
  CHECK(n==42u+bytes);CHECK(checksum_valid(packet+14,20));
  count=0;CHECK(sf_net_parse_udp(packet,n,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,dport,parsed,&count)&&count==bytes&&!memcmp(parsed,sent,bytes));
  CHECK(!sf_net_parse_udp(packet,n,SF_NET_IPV4_LEN,SF_NET_UDP_PORT,(uint16_t)(dport+1u),parsed,&count));
 }
 uint32_t tseq=0,tack=0;uint8_t tflags=0;uint16_t twindow=0;
 n=sf_net_build_tcp(packet,mac,peer,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,SF_NET_GATEWAY,9001,1234,0,SF_NET_TCP_SYN,SF_NET_TCP_WINDOW,0,0);
 CHECK(n==54);
 {
  const uint8_t*tip=packet+14;const uint8_t*ttcp=tip+20;
  CHECK(get16(packet+12)==SF_NET_IPV4&&!memcmp(packet,peer,6)&&!memcmp(packet+6,mac,6));
  CHECK(tip[0]==0x45&&get16(tip+2)==40&&tip[8]==64&&tip[9]==SF_NET_TCP&&get32(tip+12)==SF_NET_IPV4_LEN&&get32(tip+16)==SF_NET_GATEWAY);
  CHECK(checksum_valid(tip,20));
  CHECK(get16(ttcp)==SF_NET_TCP_PORT&&get16(ttcp+2)==9001&&get32(ttcp+4)==1234&&get32(ttcp+8)==0);
  CHECK(ttcp[12]==0x50&&ttcp[13]==SF_NET_TCP_SYN&&get16(ttcp+14)==SF_NET_TCP_WINDOW);
  CHECK(sf_net_tcp_checksum(SF_NET_IPV4_LEN,SF_NET_GATEWAY,ttcp,20)==0);
 }
 count=0;
 CHECK(sf_net_parse_tcp(packet,n,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,9001,&tseq,&tack,&tflags,&twindow,parsed,&count));
 CHECK(tseq==1234&&tack==0&&tflags==SF_NET_TCP_SYN&&twindow==SF_NET_TCP_WINDOW&&count==0);
 CHECK(!sf_net_parse_tcp(packet,n,SF_NET_GATEWAY,SF_NET_TCP_PORT,9001,&tseq,&tack,&tflags,&twindow,parsed,&count));
 CHECK(!sf_net_parse_tcp(packet,n,SF_NET_IPV4_LEN,9002,9001,&tseq,&tack,&tflags,&twindow,parsed,&count));
 CHECK(!sf_net_parse_tcp(packet,n,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,9002,&tseq,&tack,&tflags,&twindow,parsed,&count));
 CHECK(!sf_net_parse_tcp(packet,n-1,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,9001,&tseq,&tack,&tflags,&twindow,parsed,&count));
 n=sf_net_build_tcp(packet,mac,peer,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,SF_NET_GATEWAY,9001,1234,5678,SF_NET_TCP_ACK,SF_NET_TCP_WINDOW,text5,5);
 CHECK(n==59);
 CHECK(checksum_valid(packet+14,20)&&sf_net_tcp_checksum(SF_NET_IPV4_LEN,SF_NET_GATEWAY,packet+34,25)==0);
 count=0;
 CHECK(sf_net_parse_tcp(packet,n,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,9001,&tseq,&tack,&tflags,&twindow,parsed,&count));
 CHECK(tseq==1234&&tack==5678&&tflags==SF_NET_TCP_ACK&&twindow==SF_NET_TCP_WINDOW&&count==5&&!memcmp(parsed,text5,5));
 /* Mandatory TCP checksum: a corrupted one must reject. */
 put16(packet+50,0x1234);CHECK(!sf_net_parse_tcp(packet,n,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,9001,&tseq,&tack,&tflags,&twindow,parsed,&count));
 n=sf_net_build_tcp(packet,mac,peer,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,SF_NET_GATEWAY,9001,1234,5678,SF_NET_TCP_ACK,SF_NET_TCP_WINDOW,text5,5);
 packet[46]=0x40;CHECK(!sf_net_parse_tcp(packet,n,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,9001,&tseq,&tack,&tflags,&twindow,parsed,&count));
 n=sf_net_build_tcp(packet,mac,peer,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,SF_NET_GATEWAY,9001,1234,5678,SF_NET_TCP_ACK,SF_NET_TCP_WINDOW,text5,5);
 packet[46]=0x60;CHECK(!sf_net_parse_tcp(packet,n,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,9001,&tseq,&tack,&tflags,&twindow,parsed,&count));
 n=sf_net_build_tcp(packet,mac,peer,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,SF_NET_GATEWAY,9001,1234,5678,SF_NET_TCP_ACK,SF_NET_TCP_WINDOW,text5,5);
 put16(packet+20,0x2000);put16(packet+24,0);put16(packet+24,sf_net_checksum(0,packet+14,20));
 CHECK(!sf_net_parse_tcp(packet,n,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,9001,&tseq,&tack,&tflags,&twindow,parsed,&count));
 n=sf_net_build_tcp(packet,mac,peer,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,SF_NET_GATEWAY,9001,1234,5678,SF_NET_TCP_ACK,SF_NET_TCP_WINDOW,text5,5);
 packet[23]=SF_NET_UDP;put16(packet+24,0);put16(packet+24,sf_net_checksum(0,packet+14,20));
 CHECK(!sf_net_parse_tcp(packet,n,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,9001,&tseq,&tack,&tflags,&twindow,parsed,&count));
 CHECK(sf_net_build_tcp(back,peer,mac,SF_NET_GATEWAY,9001,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,1,2,SF_NET_TCP_ACK,SF_NET_TCP_WINDOW,sent,SF_NET_PAYLOAD_MAX+1)==0);
 uint32_t seed3=20261030u;
 for(unsigned i=0;i<1000;i++){
  seed3=seed3*1664525u+1013904223u;unsigned bytes=(unsigned)(seed3%249u);
  for(unsigned j=0;j<bytes;j++){seed3=seed3*1664525u+1013904223u;sent[j]=(uint8_t)(seed3>>24);}
  uint32_t target=0x0a000200u|(seed3&0xffu);uint16_t dport=(uint16_t)((seed3>>16)|1u);
  uint32_t s=seed3^0x5a5a5a5au,a=seed3&0x00ffffffu;uint8_t flags=(uint8_t)(seed3>>8);
  n=sf_net_build_tcp(packet,mac,peer,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,target,dport,s,a,flags,(uint16_t)(seed3>>16),sent,bytes);
  CHECK(n==54u+bytes);CHECK(checksum_valid(packet+14,20)&&sf_net_tcp_checksum(SF_NET_IPV4_LEN,target,packet+34,20u+bytes)==0);
  count=0;
  CHECK(sf_net_parse_tcp(packet,n,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,dport,&tseq,&tack,&tflags,&twindow,parsed,&count));
  CHECK(tseq==s&&tack==a&&tflags==flags&&count==bytes&&!memcmp(parsed,sent,bytes));
  CHECK(!sf_net_parse_tcp(packet,n,SF_NET_IPV4_LEN,SF_NET_TCP_PORT,(uint16_t)(dport+1u),&tseq,&tack,&tflags,&twindow,parsed,&count));
 }
 printf("NET PASS: %u checks, bounded IPv4/ARP/ICMP/UDP/TCP framing and checksum validation\n",checks);return 0;
}
