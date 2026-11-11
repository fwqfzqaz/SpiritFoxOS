#include <stdio.h>
#define SF_FILE_SYSCALL route
#include "../../services/file_client.c"
#include "file_rpc.h"
static SfNamespace*ns;
static uint32_t root;
static SfMessage pending;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("PROJECTION WIDTH FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static long provide(unsigned id,uint64_t offset,void*out,size_t bytes){
 if(id!=1)return SF_NOT_FOUND;if(offset>=6)return 0;if(bytes>6-offset)bytes=(size_t)(6-offset);
 memcpy(out,&"clock\n"[offset],bytes);return(long)bytes;
}
int64_t route(uint64_t op,uint64_t capability,uint64_t buffer,uint64_t deadline){
 (void)deadline;SfMessage*m=(void*)(uintptr_t)buffer;
 if(op==SYS_TICKS)return 10;
 if(op==SYS_SEND){if(capability!=0x101)return SF_STALE_HANDLE;pending=*m;sf_file_dispatch(ns,42,&root,&pending);pending.h.flags=1;return 0;}
 if(op==SYS_RECV){if(capability!=0x102)return SF_STALE_HANDLE;*m=pending;return 1;}
 return SF_INVALID;
}
int main(int argc,char**argv){
 ns=sf_ns_default();sf_ns_init(ns,0);CHECK(!sf_ns_create(ns,"/proc",NODE_DIR,0));
 CHECK(!sf_ns_project(ns,"/proc/clock",1));sf_ns_projection_backend(ns,provide);
 int h=sf_ns_open(ns,42,"/","/proc/clock",OPEN_READ);CHECK(h>0);
 SfFileClient client;CHECK(!sf_file_client_init(&client,0x101,0x102,3000));
 SfMessage m={0};SfFileStat stat;
 if(argc>1&&!strcmp(argv[1],"--observe")){
  int64_t direct=sf_ns_seek(ns,42,(uint32_t)h,UINT32_MAX);
  m.h.opcode=OP_SEEK;m.handle=(uint32_t)h;m.offset=UINT32_MAX;
  int64_t dispatch=sf_file_dispatch(ns,42,&root,&m);sf_ns_stat(ns,42,(uint32_t)h,&stat);
  int64_t sdk=sf_file_stat(&client,(uint32_t)h,&stat);
  printf("{\"namespace_seek\":%lld,\"dispatcher_seek\":%lld,\"wire_status\":%lld,\"sdk_stat\":%lld,\"sdk_error\":%lld}\n",
   (long long)direct,(long long)dispatch,(long long)m.status,(long long)sdk,(long long)client.error);return 0;
 }
 int duplicate=sf_ns_dup(ns,42,(uint32_t)h);CHECK(duplicate>0);
 const uint32_t edges[]={0,1,0x7fffffffu,0x80000000u,0xfffffffeu,UINT32_MAX};uint32_t random=0x13579bdf;
 for(unsigned iteration=0;iteration<1006;iteration++){
  random=random*1664525u+1013904223u;uint32_t position=iteration<6?edges[iteration]:random;
  CHECK(sf_ns_seek(ns,42,(uint32_t)h,position)==(int64_t)position);
  CHECK(!sf_ns_stat(ns,42,(uint32_t)duplicate,&stat)&&stat.offset==position);
  memset(&m,0,sizeof(m));m.h.opcode=OP_SEEK;m.handle=(uint32_t)duplicate;m.offset=position;
  CHECK(sf_file_dispatch(ns,42,&root,&m)==(int64_t)position&&m.status==(int64_t)position);
  CHECK(sf_file_seek(&client,(uint32_t)h,position)==(int64_t)position);
  CHECK(!sf_file_stat(&client,(uint32_t)duplicate,&stat)&&stat.type==NODE_PROJECTION&&stat.offset==position&&stat.size==0&&stat.flags==SF_FILE_READ_ONLY);
  uint8_t out=0xa5;int64_t n=sf_file_read(&client,(uint32_t)h,&out,1);uint64_t cursor=position;
  if(position<6){CHECK(n==1&&out==(uint8_t)"clock\n"[position]);cursor++;}else CHECK(n==0&&out==0xa5);
  CHECK(!sf_file_stat(&client,(uint32_t)duplicate,&stat)&&stat.offset==cursor);
  CHECK(sf_file_pread(&client,(uint32_t)duplicate,&out,1,0)==1&&out=='c');
  CHECK(!sf_file_stat(&client,(uint32_t)h,&stat)&&stat.offset==cursor);
  if(iteration%97==0){
   CHECK(sf_file_seek(&client,(uint32_t)h,(uint64_t)UINT32_MAX+1)==SF_INVALID);
   CHECK(sf_file_seek(&client,(uint32_t)h,UINT64_MAX)==SF_INVALID);
   CHECK(!sf_file_stat(&client,(uint32_t)h,&stat)&&stat.offset==cursor&&!client.error);
   memset(&m,0,sizeof(m));m.h.opcode=OP_SEEK;m.handle=(uint32_t)h;m.offset=0;
   CHECK(sf_file_dispatch(ns,99,&root,&m)==SF_STALE_HANDLE);
   m.handle=(uint32_t)h|(1ull<<32);CHECK(sf_file_dispatch(ns,42,&root,&m)==SF_STALE_HANDLE);
   CHECK(!sf_ns_stat(ns,42,(uint32_t)duplicate,&stat)&&stat.offset==cursor);
  }
 }
 CHECK(sf_file_root(&client)==SF_DENIED&&!client.error);
 CHECK(!sf_file_close(&client,(uint32_t)h));memset(&stat,0xa5,sizeof(stat));SfFileStat saved=stat;
 CHECK(sf_file_stat(&client,(uint32_t)h,&stat)==SF_STALE_HANDLE&&!memcmp(&stat,&saved,sizeof(stat))&&!client.error);
 CHECK(sf_file_seek(&client,(uint32_t)duplicate,1)==1);CHECK(!sf_file_close(&client,(uint32_t)duplicate));
 printf("PROJECTION WIDTH PASS: %u assertions\n",checks);return 0;
}
