#include "core_fixture.h"
static long stats(uint64_t output,uint64_t bytes,uint64_t reserved){Frame f={0};f.rax=SYS_IPC_STATS;f.rdi=output;f.rsi=bytes;f.rdx=reserved;return syscall(&f);}
int main(void){
 (void)plan;
 struct {uint64_t before;SfIpcStats value;uint64_t after;} packet;
 reset(42);memcpy(saved,tasks,sizeof(tasks));
 for(unsigned trial=0;trial<1000;trial++){
  memset(&packet,0xa5,sizeof(packet));unsigned calls=core_copy_out_calls;
  CHECK(stats((uintptr_t)&packet.value,trial%2?23:25,0)==SF_INVALID&&core_copy_out_calls==calls&&!memcmp(saved,tasks,sizeof(tasks)));
  CHECK(stats((uintptr_t)&packet.value,24,trial+1)==SF_INVALID&&core_copy_out_calls==calls);
  CHECK(stats((uintptr_t)&packet.value,24,0)==0&&core_copy_out_calls==calls+1);
  CHECK(packet.before==0xa5a5a5a5a5a5a5a5ull&&packet.after==packet.before&&packet.value.size==24&&packet.value.version==1);
  CHECK(packet.value.send_bytes==0x100000001ull&&packet.value.receive_bytes==UINT64_MAX-32&&!memcmp(saved,tasks,sizeof(tasks)));
  core_copy_out_denied=1;memset(&packet,0xa5,sizeof(packet));CHECK(stats((uintptr_t)&packet.value,24,0)==SF_DENIED&&packet.value.size==0xa5a5a5a5u);core_copy_out_denied=0;
 }
 current=1;tasks[1].pid=2;tasks[1].state=T_READY;unsigned calls=core_copy_out_calls;
 CHECK(stats((uintptr_t)&packet.value,24,0)==SF_DENIED&&stats(0,UINT64_MAX,UINT64_MAX)==SF_DENIED&&core_copy_out_calls==calls);
 printf("IPC STATS CONTROL PASS: %u checks; root-only fixed-width snapshot, 1000 invalid/valid/copy-failure vectors, guards/full64bit values and ordinary denial before parameters\n",checks);return 0;
}
