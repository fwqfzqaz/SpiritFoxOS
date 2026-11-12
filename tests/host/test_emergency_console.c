#include <stdio.h>
#include <stdlib.h>
#define SF_FILE_SYSCALL model_syscall
#include "../../services/file_client.c"
#include "emergency_console.h"
static SfEmergencyConsole console;
static uint32_t generation;
static unsigned checks,bytes_written,send_count;
static uint8_t last[256];
static SfMessage reply;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"EMERGENCY CONSOLE FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static long output(const void*data,size_t n){CHECK(n<=256);memcpy(last,data,n);bytes_written+=(unsigned)n;return(long)n;}
int64_t model_syscall(uint64_t op,uint64_t cap,uint64_t data,uint64_t deadline){
 if(op==SYS_TICKS)return 10;
 SfMessage*m=(void*)(uintptr_t)data;
 if(op==SYS_SEND){CHECK(cap==0x101&&!deadline);send_count++;reply=*m;reply.h.flags=42;sf_emergency_console(&console,&generation,42,&reply,output);return 0;}
 CHECK(op==SYS_RECV&&cap==0x102);*m=reply;m->h.flags=1;return 1;
}
static SfMessage request(unsigned op,uint64_t handle){SfMessage m={0};m.h=(SfIpcHeader){40,1,0,op,42,1,sizeof(m)-40,0,0};m.handle=handle;return m;}
int main(void){
 uint32_t old=0;
 for(unsigned i=0;i<1000;i++){
  SfFileClient client;CHECK(!sf_file_client_init(&client,0x101,0x102,3000));
  int64_t h=sf_file_console(&client);CHECK(h>0&&h!=old&&((uint32_t)h&255)==255);old=(uint32_t)h;
  CHECK(sf_file_root(&client)==SF_DENIED&&sf_file_openat(&client,(uint64_t)h,"secret",SF_OPEN_READ)==SF_INVALID);
  CHECK(sf_file_write(&client,(uint64_t)h,"hello",5)==5&&!memcmp(last,"hello",5));
  SfFileStat info;CHECK(!sf_file_stat(&client,(uint64_t)h,&info)&&info.type==SF_TYPE_CONSOLE&&info.mode==SF_OPEN_WRITE&&!info.size&&!info.flags);
  CHECK(sf_file_read(&client,(uint64_t)h,last,1)==SF_DENIED&&sf_file_seek(&client,(uint64_t)h,0)==SF_NOT_SEEKABLE&&sf_file_poll(&client,(uint64_t)h)==2);
  CHECK(sf_file_dup(&client,(uint64_t)h)==SF_NO_SPACE);
  SfMessage foreign=request(OP_WRITE,(uint64_t)h);foreign.h.flags=43;foreign.length=1;unsigned before=bytes_written;
  CHECK(sf_emergency_console(&console,&generation,43,&foreign,output)==SF_STALE_HANDLE&&bytes_written==before);
  CHECK(!sf_file_close(&client,(uint64_t)h)&&!console.handle);
  memset(&info,0xa5,sizeof(info));SfFileStat original=info;
  CHECK(sf_file_stat(&client,(uint64_t)h,&info)==SF_STALE_HANDLE&&!memcmp(&info,&original,sizeof(info))&&!client.error);
 }
 SfMessage m=request(OP_OPEN,0);memcpy(m.data,"/dev/console",13);m.length=13;m.mode=2;
 CHECK(!sf_emergency_console(&console,&generation,42,&m,output));uint32_t active=console.handle;
 for(unsigned bad=0;bad<12;bad++){
  m=request(OP_WRITE,active);m.length=1;
  switch(bad){case 0:m.h.size--;break;case 1:m.h.protocol_major++;break;case 2:m.h.protocol_minor++;break;case 3:m.h.flags++;break;
  case 4:m.h.payload_bytes--;break;case 5:m.h.cap_count=1;break;case 6:m.status=1;break;case 7:m.length=sizeof(m.data)+1;break;
  case 8:m.mode=1;break;case 9:m.offset=1;break;case 10:m.handle|=1ull<<32;break;case 11:m.handle=0;break;}
  SfEmergencyConsole saved=console;uint32_t sequence=generation;unsigned before=bytes_written;
  CHECK(sf_emergency_console(&console,&generation,42,&m,output)<0&&console.handle==saved.handle&&console.owner==saved.owner&&generation==sequence&&bytes_written==before);
 }
 m=request(OP_CLOSE,active);CHECK(!sf_emergency_console(&console,&generation,42,&m,output));
 generation=0x7fffff;m=request(OP_OPEN,0);memcpy(m.data,"/dev/console",13);m.length=13;m.mode=2;
 CHECK(sf_emergency_console(&console,&generation,42,&m,output)==SF_NO_SPACE&&!console.handle&&generation==0x7fffff);
 CHECK(send_count>10000&&bytes_written==5000);
 printf("Emergency console tests passed: %u assertions; real file SDK, bounded write-only tokens, no root/alias/cross-owner access, generation exhaustion and malformed request no side effect\n",checks);return 0;
}
