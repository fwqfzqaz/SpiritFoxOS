#include <stdio.h>
#include "../../services/namespace_handles.h"
static SfNsHandleMap map;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("NS RESTART FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int bind(uint32_t owner,int remote){
 SfNsHandleReservation r;int status=sf_nsh_prepare(&map,owner,&r);
 return status?status:sf_nsh_commit(&map,&r,(uint32_t)remote);
}
int main(void){
 SfNamespace*ns=sf_ns_default();sf_nsh_init(&map);
 uint32_t previous_local=0,previous_remote=0;
 for(unsigned boot=0;boot<1000;boot++){
  /* The real server starts with zero generations. Demonstrate the alias
   * rather than assuming remote handle values remain unique across death. */
  sf_ns_init(ns,0);CHECK(!sf_ns_create(ns,"/state",NODE_FILE,0));
  int raw=sf_ns_open(ns,31,"/","/state",OPEN_READ|OPEN_WRITE);CHECK(raw>0);
  if(boot)CHECK((uint32_t)raw==previous_remote);
  int local=bind(31,raw);CHECK(local>0);
  if(boot)CHECK((uint32_t)local!=previous_local);
  uint32_t translated=0xdeadbeef;
  if(boot){
   CHECK(sf_nsh_lookup(&map,31,previous_local,&translated)==SF_STALE_HANDLE);
   CHECK(translated==0xdeadbeef);
  }
  CHECK(!sf_nsh_lookup(&map,31,(uint32_t)local,&translated));
  CHECK(translated==(uint32_t)raw);
  CHECK(sf_ns_write(ns,31,translated,"abc",3)==3);
  CHECK(sf_ns_seek(ns,31,translated,0)==0);
  int duplicate_raw=sf_ns_dup(ns,31,translated);CHECK(duplicate_raw>0);
  int duplicate=bind(31,duplicate_raw);CHECK(duplicate>0&&duplicate!=local);
  uint32_t second=0;CHECK(!sf_nsh_lookup(&map,31,(uint32_t)duplicate,&second));
  char text[4]={0};CHECK(sf_ns_read(ns,31,second,text,1)==1&&text[0]=='a');
  CHECK(sf_ns_read(ns,31,translated,text,2)==2&&!memcmp(text,"bc",2));
  /* Handoff must be confirmed at both owners; it keeps the server's shared
   * description/offset. Map operations alone confer no server authority. */
  CHECK(!sf_ns_handoff(ns,31,second,32));
  CHECK(!sf_nsh_handoff(&map,31,(uint32_t)duplicate,32));
  CHECK(sf_nsh_lookup(&map,31,(uint32_t)duplicate,&second)==SF_STALE_HANDLE);
  CHECK(!sf_nsh_lookup(&map,32,(uint32_t)duplicate,&second));
  CHECK(sf_ns_seek(ns,32,second,1)==1);
  CHECK(sf_ns_read(ns,31,translated,text,1)==1&&text[0]=='b');
  sf_ns_reap(ns,32);sf_nsh_reap(&map,32);
  CHECK(sf_nsh_lookup(&map,32,(uint32_t)duplicate,&second)==SF_STALE_HANDLE);
  CHECK(sf_ns_read(ns,31,translated,text,1)==1&&text[0]=='c');
  previous_local=(uint32_t)local;previous_remote=(uint32_t)raw;
  /* Death loses every server description. Manager invalidation must happen
   * before accepting a new peer, without resetting its generation history. */
  sf_nsh_invalidate(&map);
  CHECK(sf_nsh_lookup(&map,31,previous_local,&translated)==SF_STALE_HANDLE);
 }
 printf("Namespace server-restart tests passed: %u assertions\n",checks);return 0;
}
