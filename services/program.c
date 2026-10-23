#include "libsf.h"
#include "namespace.h"
#if ROLE == 2
#include "package.h"
#include "trust.h"
static uint8_t native_upload[128*1024];
static uint32_t native_have,native_expected;
#endif
static __attribute__((unused)) int rpc(const SfBootArgs*a,SfMessage*m){long r=sf_send(a->manager,m);if(r)return r;r=sf_recv(a->service,m,sf_deadline(3000));return r<0?r:(int)m->status;}
long sf_main(const SfBootArgs*a,size_t bytes){
 if(bytes!=sizeof(*a)||a->size!=sizeof(*a)||a->abi_major!=1)return SF_ABI_MISMATCH;
#if ROLE == 1
 for(;;){SfMessage m;long sender=sf_recv(a->service,&m,0);if(sender<0)return sender;if(m.h.opcode!=OP_ECHO)continue;m.status=0;sf_send(a->manager,&m);}
#elif ROLE == 2
 for(;;){SfMessage m;long status=sf_recv(a->service,&m,0);if(status<0)return status;if(status!=1){m.status=SF_DENIED;sf_send(a->manager,&m);continue;}
  if(m.h.opcode==OP_NATIVE_STAGE){status=0;if(!m.offset){native_have=0;native_expected=0;if(!m.handle||m.handle>sizeof(native_upload))status=SF_INVALID;else native_expected=m.handle;}
   if(!status&&(!native_expected||m.handle!=native_expected||m.offset!=native_have||!m.length||m.length>sizeof(m.data)||m.length>native_expected-native_have))status=SF_INVALID;
   if(!status){memcpy(native_upload+native_have,m.data,m.length);native_have+=m.length;}else native_have=native_expected=0;m.status=status;m.length=0;m.offset=native_have;sf_send(a->manager,&m);continue;}
  if(m.h.opcode!=OP_NATIVE_PREPARE)continue;status=0;SfNativePlan plan={0};const uint8_t*d=0;uint64_t n=0;SfPackage p;char id[96];
  if(!m.length||m.length>sizeof(m.data)||m.data[m.length-1])status=SF_INVALID;
  else if(m.mode==1){if(!native_expected||native_have!=native_expected)status=SF_BAD_FORMAT;else{d=native_upload;n=native_have;}}
  else if(m.mode)status=SF_INVALID;
  else status=sf_bundle_find((void*)a->bundle,a->bundle_bytes,(char*)m.data,&d,&n);
  if(!status)status=sf_package_open(&p,d,n,sf_trust_key);
  if(!status&&(p.kind!=2||sf_manifest_text(&p,"runtime_id",id,sizeof(id))||strcmp(id,"org.spiritfox.native")))status=SF_BAD_FORMAT;
  if(!status){plan.entry=p.entry;for(unsigned i=0;i<p.count;i++){SfSection*s=&p.sections[i];if(s->type>3)continue;if(plan.count==5){status=SF_NO_SPACE;break;}plan.segments[plan.count++]=(SfNativeSegment){s->rva,s->memory,s->offset,s->bytes,s->flags,0};}}
  native_have=native_expected=0;m.status=status;m.length=status?0:sizeof(plan);if(!status)memcpy(m.data,&plan,sizeof(plan));sf_send(a->manager,&m);
 }
#elif ROLE == 3 || ROLE == 9
 SfMessage m;sf_message(&m,OP_OPEN,1);memcpy(m.data,"/dev/console",13);m.length=13;m.mode=OPEN_WRITE;
 int status=rpc(a,&m);if(status<0)return 1;uint32_t h=m.handle;
 sf_message(&m,OP_WRITE,2);m.handle=h;
#if ROLE == 9
 const char*text="Hello from diskhello.sfk!\n";
#else
 const char*text="Hello from hello.sfk!\n";
#endif
 m.length=strlen(text);memcpy(m.data,text,m.length);status=rpc(a,&m);if(status<0)return 2;
 sf_message(&m,OP_CLOSE,3);m.handle=h;return rpc(a,&m)<0?3:0;
#elif ROLE == 4
 *(volatile uint64_t*)0x100000=0;return 1;
#elif ROLE == 5
 __asm__ volatile("ud2");return 1;
#elif ROLE == 6
 volatile uint64_t counter=0;for(;;)counter++;
#elif ROLE == 7
 uint8_t code[]={0x90,0xc3};((void(*)(void))code)();return 1;
#elif ROLE == 8
 int ok=sf_sys(SYS_SPAWN,0,0,0)==SF_DENIED&&sf_sys(SYS_LOG,0x100000,16,0)==SF_DENIED&&sf_sys(SYS_MAP_TEST,SF_BASE,7,0)==SF_DENIED&&sf_sys(SYS_SEND,0x1234,0,0)==SF_STALE_HANDLE&&sf_sys(SYS_CAP_DERIVE,a->manager,7,0)==SF_DENIED&&sf_sys(SYS_PCI_READ,0,0,0)==SF_DENIED&&sf_sys(SYS_PORT_IO,0xcf8,4,0)==SF_DENIED;
 ok=ok&&sf_sys(SYS_DMA_STATUS,0,0,0)==SF_DENIED;SfMessage m;sf_message(&m,OP_HELLO,1);m.status=ok?0:SF_DENIED;sf_send(a->manager,&m);return ok?0:1;
#elif ROLE == 10
 return *(volatile uint32_t*)0xfed90000ull;
#elif ROLE == 11
 return 42;
#else
 return SF_INVALID;
#endif
}
