#include "libsf.h"
#include "namespace.h"
#include "arguments.h"
#if ROLE == 2
#include "package.h"
#include "trust.h"
static uint8_t native_upload[128*1024];
static uint32_t native_have,native_expected;
#endif
static __attribute__((unused)) int rpc(const SfBootArgs*a,SfMessage*m){long r=sf_send(a->manager,m);if(r)return r;r=sf_recv(a->service,m,sf_deadline(3000));return r<0?r:(int)m->status;}
#if ROLE == 14 || ROLE == 15
static int file_metadata(const SfBootArgs*a,SfMessage*m,uint32_t handle,SfFileStat*out){
 sf_message(m,OP_STAT,1);m->handle=handle;int r=rpc(a,m);if(r)return r;
 if(m->handle!=handle||m->length!=sizeof(*out))return SF_BAD_FORMAT;
 memcpy(out,m->data,sizeof(*out));return out->bytes==sizeof(*out)&&out->type>=NODE_DIR&&out->type<=NODE_DISK&&!(out->mode&~15u)&&!(out->flags&~7u)?0:SF_BAD_FORMAT;
}
static int relative_open(const SfBootArgs*a,SfMessage*m,uint32_t root,const char*path,unsigned mode){sf_message(m,OP_OPENAT,1);m->handle=root;m->length=strlen(path)+1;m->mode=mode;memcpy(m->data,path,m->length);return rpc(a,m);}
#endif
long sf_main(const SfBootArgs*a,size_t bytes){
 if(sf_boot_arguments_validate(a,bytes))return SF_ABI_MISMATCH;
#if ROLE == 1
 unsigned bad_reply=0;
 for(;;){SfMessage m;long sender=sf_recv(a->service,&m,0);if(sender<0)return sender;
  if(m.h.opcode==OP_ECHO_DIAGNOSTIC){if(sender!=1)continue;if(m.mode==1)for(;;)__asm__ volatile("pause");if(m.mode==2||m.mode==3)bad_reply=m.mode;continue;}
  if(m.h.opcode!=OP_ECHO)continue;m.status=bad_reply==2?SF_DENIED:0;if(bad_reply==3)m.h.opcode=OP_HELLO;bad_reply=0;sf_send(a->manager,&m);
 }
#elif ROLE == 2
 unsigned bad_health=0;
 for(;;){SfMessage m;long status=sf_recv(a->service,&m,0);if(status<0)return status;if(status!=1){m.status=SF_DENIED;sf_send(a->manager,&m);continue;}
  if(m.h.opcode==OP_NATIVE_DIAGNOSTIC){if(m.mode==1)for(;;)__asm__ volatile("pause");if(m.mode==2||m.mode==3)bad_health=m.mode;continue;}
  if(m.h.opcode==OP_NATIVE_HEALTH){m.status=bad_health==2?SF_DENIED:0;if(bad_health==3)m.h.opcode=OP_HELLO;bad_health=0;m.length=0;sf_send(a->manager,&m);continue;}
  if(m.h.opcode==OP_NATIVE_STAGE){status=0;if(!m.offset){native_have=0;native_expected=0;if(!m.handle||m.handle>sizeof(native_upload))status=SF_INVALID;else native_expected=m.handle;}
   if(!status&&(!native_expected||m.handle!=native_expected||m.offset!=native_have||!m.length||m.length>sizeof(m.data)||m.length>native_expected-native_have))status=SF_INVALID;
   if(!status){memcpy(native_upload+native_have,m.data,m.length);native_have+=m.length;}else native_have=native_expected=0;m.status=status;m.length=0;m.offset=native_have;sf_send(a->manager,&m);continue;}
  if(m.h.opcode!=OP_NATIVE_PREPARE)continue;status=0;SfNativePlan plan={0};const uint8_t*d=0;uint64_t n=0;SfPackage p;char id[96];
  if(!m.length||m.length>sizeof(m.data)||m.data[m.length-1])status=SF_INVALID;
  else if(m.mode==1){if(!native_expected||native_have!=native_expected)status=SF_BAD_FORMAT;else{d=native_upload;n=native_have;}}
  else if(m.mode)status=SF_INVALID;
  else status=sf_bundle_find((void*)a->bundle,a->bundle_bytes,(char*)m.data,&d,&n);
  if(!status)status=sf_package_open_trusted(&p,d,n,&sf_trust_store);
  if(!status&&(p.kind!=2||sf_manifest_text(&p,"runtime_id",id,sizeof(id))||strcmp(id,"org.spiritfox.native")))status=SF_BAD_FORMAT;
  if(!status&&(!sf_image_base_valid(m.offset)||!m.offset||(p.native_profile==SF_NATIVE_STATIC&&m.offset!=SF_BASE)||(p.native_profile==SF_NATIVE_RELATIVE&&m.offset<SF_PIE_MIN)))status=SF_INVALID;
  if(!status){plan.entry=p.entry;plan.image_base=m.offset;plan.profile=p.native_profile;plan.relocation_offset=p.relocation_offset;plan.relocation_count=p.relocation_count;for(unsigned i=0;i<p.count;i++){SfSection*s=&p.sections[i];if(s->type>3)continue;if(plan.count==5){status=SF_NO_SPACE;break;}plan.segments[plan.count++]=(SfNativeSegment){s->rva,s->memory,s->offset,s->bytes,s->flags,0};}}
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
#elif ROLE == 16
 if(a->argc>1&&!strcmp(sf_argument(a,bytes,1),"--require-mode")&&!sf_environment_value(a,bytes,"MODE"))return 42;
 if(a->argc>1&&!strcmp(sf_argument(a,bytes,1),"--write-bootargs")){*(volatile char*)sf_argument(a,bytes,0)='x';return 99;}
 if(a->argc>1&&a->envc&&!strcmp(sf_argument(a,bytes,1),"--write-environment")){*(volatile char*)sf_environment(a,bytes,0)='x';return 99;}
 SfMessage m;sf_message(&m,OP_OPEN,1);memcpy(m.data,"/dev/console",13);m.length=13;m.mode=OPEN_WRITE;if(rpc(a,&m))return 1;uint32_t output=m.handle;
 char text[SF_ARGUMENT_BYTES+160];unsigned used=0;const char*prefix="ARGC=";while(*prefix)text[used++]=*prefix++;text[used++]='0'+a->argc;text[used++]='\n';
 memcpy(text+used,"ENVC=",5);used+=5;text[used++]='0'+a->envc;text[used++]='\n';
 for(unsigned i=0;i<a->argc;i++){const char*s=sf_argument(a,bytes,i);if(!s)return 2;memcpy(text+used,"ARGV[",5);used+=5;text[used++]='0'+i;text[used++]=']';text[used++]='=';while(*s)text[used++]=*s++;text[used++]='\n';}
 for(unsigned i=0;i<a->envc;i++){const char*s=sf_environment(a,bytes,i);if(!s)return 2;memcpy(text+used,"ENV[",4);used+=4;text[used++]='0'+i;text[used++]=']';text[used++]='=';while(*s)text[used++]=*s++;text[used++]='\n';}
 for(unsigned off=0;off<used;){sf_message(&m,OP_WRITE,2);m.handle=output;m.length=used-off;if(m.length>sizeof(m.data))m.length=sizeof(m.data);unsigned sent=m.length;memcpy(m.data,text+off,sent);if(rpc(a,&m)!=(int)sent)return 3;off+=sent;}
 sf_message(&m,OP_CLOSE,3);m.handle=output;return rpc(a,&m);
#elif ROLE == 4
 *(volatile uint64_t*)0x100000=0;return 1;
#elif ROLE == 5
 __asm__ volatile("ud2");return 1;
#elif ROLE == 6
 volatile uint64_t counter=0;for(;;)counter++;
#elif ROLE == 7
 uint8_t code[]={0x90,0xc3};((void(*)(void))code)();return 1;
#elif ROLE == 8
 for(unsigned i=0;i<128;i++){long ep=sf_sys(SYS_EP_CREATE,0,0,0);if(ep<0||sf_sys(SYS_CAP_CLOSE,ep,0,0))return 9;}
 int ok=sf_sys(SYS_SPAWN,0,0,0)==SF_DENIED&&sf_sys(SYS_LOG,0x100000,16,0)==SF_DENIED&&sf_sys(SYS_MAP_TEST,SF_BASE,7,0)==SF_DENIED&&sf_sys(SYS_SEND,0x1234,0,0)==SF_STALE_HANDLE&&sf_sys(SYS_CAP_DERIVE,a->manager,7,0)==SF_DENIED&&sf_sys(SYS_PCI_READ,0,0,0)==SF_DENIED&&sf_sys(SYS_PORT_IO,0xcf8,4,0)==SF_DENIED;
 ok=ok&&sf_sys(SYS_DMA_STATUS,0,0,0)==SF_DENIED;SfMessage m;sf_message(&m,OP_HELLO,1);m.status=ok?0:SF_DENIED;sf_send(a->manager,&m);return ok?0:1;
#elif ROLE == 10
 return *(volatile uint32_t*)0xfed90000ull;
#elif ROLE == 11
 return 42;
#elif ROLE == 12
 return SF_BUSY;
#elif ROLE == 13
 __asm__ volatile("movabs $0x800000000000,%%rsp; mov $1,%%rax; int $0x80; ud2":::"memory");__builtin_unreachable();
#elif ROLE == 14 || ROLE == 15
 SfMessage m;sf_message(&m,OP_ROOT,1);if(rpc(a,&m))return 2;uint32_t root=m.handle;
#if ROLE == 14
 const char*target="notes";
#else
 const char*target="readme.txt";
#endif
 SfFileStat info;
 sf_message(&m,OP_CLOSE,1);m.handle=(1ull<<32)|root;if(rpc(a,&m)!=SF_STALE_HANDLE||m.length)return 18;
 sf_message(&m,OP_ROOT,1);if(rpc(a,&m)||m.handle!=root)return 19;
 if(file_metadata(a,&m,root,&info)||info.type!=NODE_DIR||info.mode!=(OPEN_READ|OPEN_DIRECTORY)||info.size||info.offset)return 20;
 if(relative_open(a,&m,root,target,OPEN_WRITE)!=SF_DENIED)return 3;
 const char*bad[]={"../outside","/dev/console",".","sub/../notes","notes/","sub//notes"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)if(relative_open(a,&m,root,bad[i],OPEN_READ)!=SF_INVALID)return 4;
 if(relative_open(a,&m,root^256,target,OPEN_READ)!=SF_STALE_HANDLE)return 5;
 sf_message(&m,OP_OPEN,1);memcpy(m.data,"/tmp/notes",11);m.length=11;m.mode=OPEN_READ;if(rpc(a,&m)!=SF_DENIED)return 6;
 sf_message(&m,OP_OPENAT,1);m.handle=root;m.mode=OPEN_READ;memcpy(m.data,"notes\0extra",12);m.length=12;if(rpc(a,&m)!=SF_INVALID)return 7;
 sf_message(&m,OP_ENUMERATE,1);m.handle=root;m.length=sizeof(m.data);if(rpc(a,&m)<=0)return 8;
 if(relative_open(a,&m,root,target,OPEN_READ))return 9;uint32_t file=m.handle;
 if(file_metadata(a,&m,file,&info)||(info.type!=NODE_FILE&&info.type!=NODE_DISK)||info.mode!=OPEN_READ||!info.size||info.offset)return 21;
 sf_message(&m,OP_READ,1);m.handle=(1ull<<63)|file;m.length=1;if(rpc(a,&m)!=SF_STALE_HANDLE||m.length)return 22;
 if(file_metadata(a,&m,file,&info)||info.offset)return 23;
 sf_message(&m,OP_TRUNCATE,1);m.handle=file;m.offset=0;if(rpc(a,&m)!=SF_DENIED)return 16;
 sf_message(&m,OP_UNLINK,1);m.handle=root;m.length=strlen(target)+1;memcpy(m.data,target,m.length);if(rpc(a,&m)!=SF_DENIED)return 17;
 sf_message(&m,OP_CLOSE,1);m.handle=root;if(rpc(a,&m))return 10;
 sf_message(&m,OP_ROOT,1);if(rpc(a,&m)!=SF_DENIED)return 11;
 sf_message(&m,OP_STAT,1);m.handle=root;if(rpc(a,&m)!=SF_STALE_HANDLE||m.length)return 24;
 if(relative_open(a,&m,root,target,OPEN_READ)!=SF_STALE_HANDLE)return 12;
 sf_message(&m,OP_READ,1);m.handle=file;m.length=sizeof(m.data);int read_count=rpc(a,&m);if(read_count<=0)return 13;
 if(file_metadata(a,&m,file,&info)||info.offset!=(unsigned)read_count)return 25;
 /* Deliberately leave the child file and stdout open: process reap must
  * release their namespace descriptions and any disk mount lease. */
 sf_message(&m,OP_OPEN,1);memcpy(m.data,"/dev/console",13);m.length=13;m.mode=OPEN_WRITE;if(rpc(a,&m))return 14;uint32_t console=m.handle;
 sf_message(&m,OP_WRITE,1);m.handle=console;const char*text="OPENAT APP PASSED\n";m.length=strlen(text);memcpy(m.data,text,m.length);return rpc(a,&m)<0?15:0;
#else
 return SF_INVALID;
#endif
}
