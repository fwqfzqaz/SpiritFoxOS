#include <stdio.h>
#include <stdlib.h>
#include "namespace_transport.h"
static unsigned checks,sends,receives,pumps,closes,timeouts,corrupt;
static uint64_t ticks;
static int live=1,stalled,backward,reenter,send_result,receive_result,pump_result;
static int64_t status;
static SfMessage sent;
static SfNsTransport transport;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"ROOT TRANSPORT FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static int64_t send(void*c,uint32_t cap,SfMessage*m){(void)c;CHECK(cap==0x101);sends++;sent=*m;return send_result;}
static int64_t receive(void*c,uint32_t cap,SfMessage*m,unsigned ms){
 (void)c;CHECK(cap==0x102&&ms==1);receives++;if(receive_result)return receive_result;
 if(timeouts){timeouts--;return SF_TIMEOUT;}
 *m=sent;m->h.flags=77;m->h.deadline_ns=0;m->status=status;
 switch(corrupt){case 1:return 78;case 2:m->h.request_id++;break;case 3:m->h.flags=78;break;case 4:m->h.size--;break;
 case 5:m->h.protocol_major++;break;case 6:m->h.protocol_minor++;break;case 7:m->h.opcode++;break;case 8:m->h.payload_bytes--;break;
 case 9:m->h.cap_count=1;m->handle=0x777;break;case 10:m->h.deadline_ns=1;break;}
 return 77;
}
static uint64_t clock(void*c){(void)c;if(backward){if(ticks)ticks--;return ticks;}return stalled?ticks:ticks++;}
static int alive(void*c,uint32_t peer){(void)c;CHECK(peer==77);return live;}
static void close(void*c,uint64_t h){(void)c;CHECK(h==0x777);closes++;}
static int pump(void*c,unsigned ms){(void)c;CHECK(ms==1);pumps++;if(reenter){SfMessage m=sent;unsigned calls=sends;CHECK(sf_nst_exchange(&transport,&m)==SF_BUSY&&sends==calls);}return pump_result;}
static SfMessage message(uint64_t id){SfMessage m={0};m.h=(SfIpcHeader){40,1,0,222,0,id,sizeof(m)-40,0,0};return m;}
static void reset(void){
 sends=receives=pumps=closes=timeouts=corrupt=0;ticks=100;live=1;stalled=backward=reenter=send_result=receive_result=pump_result=0;status=0;
 transport=(SfNsTransport){{send,receive,clock,alive,pump,close},0,0x101,0x102,77,0};
}
int main(void){
 for(unsigned i=0;i<1000;i++){
  reset();timeouts=i%9;reenter=1;status=i&1?SF_NOT_FOUND:UINT32_MAX;SfMessage m=message((1ull<<32)+i);
  CHECK(sf_nst_exchange(&transport,&m)==77&&m.status==status&&m.h.request_id==(1ull<<32)+i);
  CHECK(!transport.inflight&&sends==1&&receives==pumps+1&&pumps==i%9);
 }
 for(corrupt=1;corrupt<=10;corrupt++){
  unsigned kind=corrupt;reset();corrupt=kind;SfMessage m=message(1);CHECK(sf_nst_exchange(&transport,&m)==(kind<=2?SF_STALE_HANDLE:SF_BAD_FORMAT));CHECK(!transport.inflight&&closes==(kind==9));
 }
 reset();live=0;SfMessage m=message(1);CHECK(sf_nst_exchange(&transport,&m)==SF_DEVICE_GONE&&!sends&&!transport.inflight);
 reset();send_result=SF_WOULD_BLOCK;m=message(1);CHECK(sf_nst_exchange(&transport,&m)==SF_WOULD_BLOCK&&!receives&&!transport.inflight);
 reset();send_result=1;m=message(1);CHECK(sf_nst_exchange(&transport,&m)==SF_BAD_FORMAT&&!receives&&!transport.inflight);
 reset();receive_result=SF_REVOKED;m=message(1);CHECK(sf_nst_exchange(&transport,&m)==SF_REVOKED&&!transport.inflight);
 reset();timeouts=1;pump_result=SF_DENIED;m=message(1);CHECK(sf_nst_exchange(&transport,&m)==SF_DENIED&&pumps==1&&!transport.inflight);
 reset();timeouts=10000;m=message(1);CHECK(sf_nst_exchange(&transport,&m)==SF_TIMEOUT&&receives<6000&&!transport.inflight);
 reset();stalled=1;timeouts=10000;m=message(1);CHECK(sf_nst_exchange(&transport,&m)==SF_TIMEOUT&&receives==6000&&!transport.inflight);
 reset();backward=1;m=message(1);CHECK(sf_nst_exchange(&transport,&m)==SF_BAD_FORMAT&&!receives&&!transport.inflight);
 reset();ticks=UINT64_MAX;m=message(1);CHECK(sf_nst_exchange(&transport,&m)==SF_INVALID&&!receives&&!transport.inflight);
 reset();m=message(0);CHECK(sf_nst_exchange(&transport,&m)==SF_INVALID&&!sends);CHECK(sf_nst_exchange(0,&m)==SF_INVALID);CHECK(sf_nst_exchange(&transport,0)==SF_INVALID);
 printf("Namespace Root-transport tests passed: %u assertions; semantic status/sender separation, full nonce, callback pumping/reentry, bounded timeout/death, cap cleanup; host exchange model\n",checks);return 0;
}
