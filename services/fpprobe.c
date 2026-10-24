#include "libsf.h"
#include "arguments.h"

typedef struct {uint8_t bytes[512];} __attribute__((aligned(16))) State;
static void save(State*s){__asm__ volatile("fxsave64 %0":"=m"(*s)::"memory");}
static void restore(const State*s){__asm__ volatile("fxrstor64 %0"::"m"(*s):"memory");}
static uint16_t word(const State*s,unsigned off){uint16_t v;memcpy(&v,s->bytes+off,sizeof(v));return v;}
static uint32_t dword(const State*s,unsigned off){uint32_t v;memcpy(&v,s->bytes+off,sizeof(v));return v;}
static int fresh(const State*s){
 if(word(s,0)!=0x037f||word(s,2)||s->bytes[4]||word(s,6)||dword(s,24)!=0x1f80)return 0;
 for(unsigned i=8;i<24;i++)if(s->bytes[i])return 0;
 for(unsigned i=0;i<8;i++)for(unsigned j=0;j<10;j++)if(s->bytes[32+16*i+j])return 0;
 for(unsigned i=160;i<416;i++)if(s->bytes[i])return 0;
 return 1;
}
static int same(const State*a,const State*b){
 for(unsigned i=0;i<416;i++){
  if(i==5||(i>=28&&i<32)||(i>=32&&i<160&&(i-32)%16>=10))continue;
  if(a->bytes[i]!=b->bytes[i]){sf_log("CPU MISMATCH offset=");sf_number(i);sf_log(" expected=");sf_number(a->bytes[i]);sf_log(" actual=");sf_number(b->bytes[i]);sf_log("\n");return 0;}
 }
 return 1;
}
static uint64_t tsc(void){uint32_t lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));return((uint64_t)hi<<32)|lo;}
static void pattern(State*s,uint32_t pid){
 memset(s,0,sizeof(*s));uint16_t cw=0x037f|((pid&3)<<10);uint32_t mx=0x1f80|((pid&3)<<13);
 memcpy(s->bytes,&cw,2);memcpy(s->bytes+24,&mx,4);s->bytes[4]=255;
 uint16_t sw=((pid&7)<<11)|0x20,op=1+(pid&0x7fe);uint64_t ip=(uintptr_t)pattern,dp=(uintptr_t)s;
 memcpy(s->bytes+2,&sw,2);memcpy(s->bytes+6,&op,2);memcpy(s->bytes+8,&ip,8);memcpy(s->bytes+16,&dp,8);
 for(unsigned i=0;i<8;i++){uint64_t mantissa=0x8000000000000000ull|((uint64_t)pid<<8)|i;uint16_t exponent=0x3fff+i;memcpy(s->bytes+32+16*i,&mantissa,8);memcpy(s->bytes+40+16*i,&exponent,2);}
 for(unsigned i=160;i<416;i++)s->bytes[i]=(uint8_t)(pid*131+i*17+(i>>4));
}
long sf_main(const SfBootArgs*a,size_t bytes){
 State first={0};save(&first);uint32_t env[7]={0};__asm__ volatile("fnstenv %0":"=m"(env)::"memory");
 if((env[0]&0xffff)!=0x037f||(env[1]&0xffff)||(env[2]&0xffff)!=0xffff||env[3]||env[5])return 49;
 if(!fresh(&first)){sf_log("CPU STATE FRESH CHECK FAILED\n");return 40;}
 uint32_t eax,ebx,ecx,edx;__asm__ volatile("cpuid":"=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(1),"c"(0));
 if(ecx&(1u<<27))return 41; /* OSXSAVE must remain disabled in this profile. */
 const char*mode=a->argc>1?sf_argument(a,bytes,1):"";
 if(!mode)return 42;
 if(!strcmp(mode,"--require-avx-cpu")&&(ecx&((1u<<26)|(1u<<28)))!=((1u<<26)|(1u<<28)))return 43;
 if(!strcmp(mode,"--avx")){__asm__ volatile(".byte 0xc5,0xfd,0xef,0xc0");return 90;}
 if(!strcmp(mode,"--x87-fault")){
  uint16_t cw=0x037e;__asm__ volatile("fninit; fldcw %0; fldz; fldz; fdivp; fwait"::"m"(cw):"memory");return 91;
 }
 if(!strcmp(mode,"--simd-fault")){
  uint32_t mx=0x1f00;__asm__ volatile("ldmxcsr %0; pxor %%xmm0,%%xmm0; divss %%xmm0,%%xmm0"::"m"(mx):"memory");
  /* The bundled TCG records IE without delivering #XM. Keep this observable
   * limitation distinct from actual hardware exception delivery. */
  uint32_t actual_mx;__asm__ volatile("stmxcsr %0":"=m"(actual_mx));if(actual_mx!=0x1f01)return 92;
  sf_yield();__asm__ volatile("stmxcsr %0":"=m"(actual_mx));if(actual_mx!=0x1f01)return 94;
  sf_log("SIMD UNMASKED STATUS PRESERVED (TCG NO TRAP)\n");return 0;
 }
 if(!strcmp(mode,"--pending-x87")){
  State pending=first;pending.bytes[0]=0x7e;pending.bytes[2]=0x81;restore(&pending);sf_yield();__asm__ volatile("fwait");return 93;
 }
 /* Exercise an actual x87 operation before loading the private raw patterns. */
 int sum=0;__asm__ volatile("fninit; fld1; fld1; faddp; fistpl %0":"=m"(sum)::"memory");if(sum!=2)return 44;
 State seeded={0},expected={0},actual={0};pattern(&seeded,a->pid);restore(&seeded);save(&expected);
 if(!strcmp(mode,"--spin"))for(;;)__asm__ volatile("pause");
 uint64_t live_until=sf_ticks()+30;
 for(unsigned i=0;i<128;i++){sf_yield();save(&actual);if(!same(&expected,&actual))return 45;}
 while(sf_ticks()<live_until){sf_yield();save(&actual);if(!same(&expected,&actual))return 46;}
 uint64_t before=sf_ticks(),start=tsc();while(tsc()-start<600000000ull)__asm__ volatile("pause");
 save(&actual);if(!same(&expected,&actual)||sf_ticks()-before<2)return 47;
 /* MMX aliases the physical x87 payload. Test all eight after SSE/x87. */
 uint64_t mm[8];for(unsigned i=0;i<8;i++)mm[i]=0x7370697269740000ull|((uint64_t)a->pid<<8)|i;
 __asm__ volatile("movq 0(%0),%%mm0; movq 8(%0),%%mm1; movq 16(%0),%%mm2; movq 24(%0),%%mm3; movq 32(%0),%%mm4; movq 40(%0),%%mm5; movq 48(%0),%%mm6; movq 56(%0),%%mm7"::"r"(mm):"memory");
 save(&expected);for(unsigned i=0;i<32;i++){sf_yield();save(&actual);if(!same(&expected,&actual))return 48;}
 /* Leave dirty data and altered rounding controls for the next fresh task. */
 sf_log("CPU STATE APP PASSED\n");return 0;
}
