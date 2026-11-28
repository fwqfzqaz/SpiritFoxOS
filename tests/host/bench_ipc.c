/* 宿主 IPC 往返延迟基准：直接复用内核 ipc.c，在单进程内模拟两个任务
 * 通过端点收发，测量 SEND+RECV 一轮的 CPU 时间。属于 E1 证据（宿主 CPU
 * 上的机制开销），不代表 QEMU/真机上的端到端延迟。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../kernel/ipc.c"
Task tasks[SF_MAX_THREADS];int cpu_currents[SF_MAX_CPUS];uint64_t ticks;
void task_wake(Task*t){t->state=T_READY;t->ready_since=ticks;}
static Frame frames[SF_MAX_THREADS];
int copy_in(Task*t,void*out,uint64_t from,size_t n){(void)t;memcpy(out,(void*)(uintptr_t)from,n);return 0;}
int copy_out(Task*t,uint64_t to,const void*in,size_t n){(void)t;memcpy((void*)(uintptr_t)to,in,n);return 0;}
int user_range(Task*t,uint64_t a,uint64_t n,int write){(void)t;(void)a;(void)n;(void)write;return 1;}
void panic(const char*s){fprintf(stderr,"Unexpected panic: %s\n",s);abort();}

static double now(void){return (double)clock()/CLOCKS_PER_SEC;}
static void fail(const char*what){fprintf(stderr,"BENCH IPC FAIL: %s\n",what);exit(1);}

int main(int argc,char**argv){
 unsigned iterations=argc>1?(unsigned)strtoul(argv[1],0,10):200000u;
 if(!iterations)iterations=200000u;
 memset(tasks,0,sizeof(tasks));memset(frames,0,sizeof(frames));
 for(unsigned i=0;i<2;i++){
  tasks[i].pid=i+1;tasks[i].state=T_READY;tasks[i].cap_limit=SF_CAP_LIMIT_MAX;
  tasks[i].frame=&frames[i];tasks[i].main=1;tasks[i].main_slot=i;tasks[i].tid=1;
  tasks[i].thread_limit=SF_THREAD_LIMIT_MAX;tasks[i].thread_count=1;
  tasks[i].thread_slots=1;tasks[i].next_tid=1;tasks[i].cpu=-1;
 }
 current=0;ticks=1;
 long sender=ipc_syscall(SYS_EP_CREATE,0,0,0);
 if(sender<=0)fail("endpoint create");
 int receiver=ipc_grant(&tasks[0],&tasks[1],(uint32_t)sender,RIGHT_RECV);
 if(receiver<=0)fail("receiver grant");
 SfMessage in={0},out;
 in.h.size=40;in.h.protocol_major=1;in.h.payload_bytes=sizeof(in)-40;in.h.request_id=0x5150;
 /* 预热一次，确保句柄与端点状态已建立。 */
 current=0;if(ipc_syscall(SYS_SEND,(uint32_t)sender,(uintptr_t)&in,0))fail("warmup send");
 current=1;if(ipc_syscall(SYS_RECV,(uint32_t)receiver,(uintptr_t)&out,0)!=1)fail("warmup recv");
 double start=now();
 for(unsigned i=0;i<iterations;i++){
  current=0;if(ipc_syscall(SYS_SEND,(uint32_t)sender,(uintptr_t)&in,0))fail("send");
  current=1;if(ipc_syscall(SYS_RECV,(uint32_t)receiver,(uintptr_t)&out,0)!=1)fail("recv");
 }
 double seconds=now()-start;
 if(seconds<=0)seconds=1e-9;
 printf("RESULT ipc_roundtrip_ns=%.1f\n",seconds*1e9/(double)iterations);
 printf("RESULT ipc_roundtrips_per_s=%.0f\n",(double)iterations/seconds);
 printf("RESULT ipc_message_bytes=%u\n",(unsigned)sizeof(SfMessage));
 printf("RESULT ipc_iterations=%u\n",iterations);
 printf("BENCH IPC PASS: %u send/recv rounds, %.1f ns/round\n",iterations,seconds*1e9/(double)iterations);
 return 0;
}
