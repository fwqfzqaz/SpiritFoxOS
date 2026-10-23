#include "libsf.h"
#include "namespace.h"
#include "arguments.h"
extern void _sf_start(void);
static volatile uint64_t counter=7;
static volatile uint64_t *volatile data_pointer=&counter;
static uint64_t uninitialized[16];
static uint64_t *volatile bss_pointer=uninitialized;
static uint64_t increment(void){return ++counter;}
static uint64_t (*volatile function_pointer)(void)=increment;
static int rpc(const SfBootArgs*a,SfMessage*m){long r=sf_send(a->manager,m);if(r)return r;r=sf_recv(a->service,m,sf_deadline(3000));return r<0?r:(int)m->status;}
long sf_main(const SfBootArgs*a,size_t bytes){
 if(sf_boot_arguments_validate(a,bytes))return 1;
 if(data_pointer!=&counter||function_pointer!=increment||bss_pointer!=uninitialized||*data_pointer!=7)return 2;
 for(unsigned i=0;i<16;i++)if(bss_pointer[i])return 3;
 bss_pointer[15]=9;if(function_pointer()!=8||*data_pointer!=8)return 4;
 uint64_t base=(uint64_t)_sf_start;if(base!=SF_PIE_MIN&&base!=SF_PIE_MIN+0x2000000ull)return 5;
 if(a->argc>1&&!strcmp(sf_argument(a,bytes,1),"--write-code")){*(volatile uint8_t*)base=0;return 99;}
 if(a->argc>1&&!strcmp(sf_argument(a,bytes,1),"--require-mode")&&!sf_environment_value(a,bytes,"MODE"))return 42;
 SfMessage m;sf_message(&m,OP_OPEN,1);memcpy(m.data,"/dev/console",13);m.length=13;m.mode=OPEN_WRITE;if(rpc(a,&m))return 6;uint32_t h=m.handle;
 sf_message(&m,OP_WRITE,2);m.handle=h;const char*text=base==SF_PIE_MIN?"PIE PASS base=0x44000000 counter=8\n":"PIE PASS base=0x46000000 counter=8\n";m.length=strlen(text);memcpy(m.data,text,m.length);if(rpc(a,&m)<0)return 7;
 sf_message(&m,OP_CLOSE,3);m.handle=h;return rpc(a,&m)?8:0;
}
