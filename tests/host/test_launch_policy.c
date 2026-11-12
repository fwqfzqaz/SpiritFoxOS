#include <stdio.h>
#include "launch_policy.h"
static unsigned checks;static uint8_t manifest[512];static size_t used;
#define CHECK(x) do{checks++;if(!(x)){printf("LAUNCH POLICY FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static void text(const char *s){size_t n=strlen(s);if(n<24)manifest[used++]=0x60+n;else{manifest[used++]=0x78;manifest[used++]=n;}memcpy(manifest+used,s,n);used+=n;}
static void setup(SfPackage *p,const char *runtime,const char *library,const char *module,int capability){
 used=0;manifest[used++]=0xa4;text("module_id");text(module);text("runtime_id");text(runtime);
 text("native_library");text(library);text("requested_caps");manifest[used++]=capability<0?0x01:capability?0x81:0x80;
 if(capability>0)text(capability==1?"namespace.read":"namespace.write");
 memset(p,0,sizeof(*p));p->manifest=manifest;p->manifest_bytes=used;p->kind=2;
}
int main(void){
 SfPackage p,copy;SfArguments args;memset(&args,0,sizeof(args));
 CHECK(sf_launch_admit(0,2,0,0)==SF_INVALID&&sf_launch_library_request(0)==SF_INVALID&&sf_launch_library_admit(0)==SF_INVALID);
 setup(&p,"org.spiritfox.native","org.spiritfox.posix","org.spiritfox.posix",1);copy=p;
 CHECK(!sf_launch_admit(&p,2,0,0)&&!sf_launch_admit(&p,2,1,0));
 CHECK(!sf_launch_library_request(&p));
 CHECK(sf_launch_admit(&p,1,0,0)==SF_DENIED);
 CHECK(!memcmp(&p,&copy,sizeof(p)));
 args.count=1;CHECK(!sf_launch_admit(&p,2,1,&args));args.count=0;args.env_count=1;CHECK(!sf_launch_admit(&p,2,0,&args));
 p.kind=1;CHECK(sf_launch_admit(&p,2,0,0)==SF_DENIED&&sf_launch_admit(&p,1,1,0)==SF_DENIED);
 CHECK(sf_launch_admit(&p,1,0,&args)==SF_DENIED);args.env_count=0;args.count=1;CHECK(sf_launch_admit(&p,1,0,&args)==SF_DENIED);
 args.count=0;CHECK(!sf_launch_admit(&p,1,0,&args));
 p.native_profile=SF_NATIVE_RELATIVE;p.native_role=SF_NATIVE_ROLE_LIBRARY;p.export_count=1;CHECK(!sf_launch_library_admit(&p));
 p.kind=2;CHECK(sf_launch_library_admit(&p)==SF_DEPENDENCY);p.kind=1;p.native_profile=0;CHECK(sf_launch_library_admit(&p)==SF_DEPENDENCY);
 p.native_profile=SF_NATIVE_RELATIVE;p.native_role=SF_NATIVE_ROLE_PROCESS;CHECK(sf_launch_library_admit(&p)==SF_DEPENDENCY);
 p.native_role=SF_NATIVE_ROLE_LIBRARY;p.export_count=0;CHECK(sf_launch_library_admit(&p)==SF_DEPENDENCY);
 setup(&p,"other.runtime","other.library","other.module",1);CHECK(sf_launch_admit(&p,2,0,0)==SF_DEPENDENCY&&sf_launch_library_request(&p)==SF_DEPENDENCY);
 p.kind=1;p.native_profile=SF_NATIVE_RELATIVE;p.native_role=SF_NATIVE_ROLE_LIBRARY;p.export_count=1;CHECK(sf_launch_library_admit(&p)==SF_DEPENDENCY);
 for(int cap=-1;cap<=2;cap++){
  setup(&p,"org.spiritfox.native","org.spiritfox.posix","org.spiritfox.posix",cap);
  CHECK(sf_launch_admit(&p,2,1,0)==(cap==1?0:cap<0?SF_BAD_FORMAT:SF_DENIED));
 }
 setup(&p,"org.spiritfox.native","org.spiritfox.posix","org.spiritfox.posix",1);
 /* Every truncation is a real decoder boundary. No forged/stubbed manifest
  * parser is linked; the same production package.c accepts the complete map. */
 uint64_t full=p.manifest_bytes;
 for(uint64_t n=0;n<full;n++){p.manifest_bytes=n;CHECK(sf_launch_admit(&p,2,1,0)!=0);}
 p.manifest_bytes=full;CHECK(!sf_launch_admit(&p,2,1,0));
 for(unsigned i=0;i<1000;i++){
  p.kind=(i&1)?1:2;CHECK(sf_launch_admit(&p,2,1,0)==((i&1)?SF_DENIED:0));
  CHECK(!sf_launch_library_request(&p));
 }
 printf("LAUNCH POLICY PASS: %u checks; authenticated kind, delegation/runtime/library metadata and truncated real CBOR\n",checks);return 0;
}
