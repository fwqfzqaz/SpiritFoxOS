#include <stdio.h>
#include "../../kernel/kernel.h"
Task tasks[SF_MAX_THREADS];
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("Compat FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
/* 描述符存放在主线程槽位；测试里同时清掉本地 Task 与 task_main 的副本。 */
static void reset(Task*t){t->compat_handler=0;t->compat_frame=0;t->compat_nested=0;t->compat_flags=0;Task*m=task_main(t);m->compat_handler=0;m->compat_frame=0;m->compat_nested=0;m->compat_flags=0;}
int main(void){
 Task a={0};a.pid=21;a.main=1;a.main_slot=0;a.page_limit=SF_PAGE_LIMIT_MAX;tasks[0].pid=21;tasks[0].main=1;tasks[0].main_slot=0;tasks[0].page_limit=SF_PAGE_LIMIT_MAX;a.cr3=space_new(a.pid);CHECK(a.cr3);
 uint8_t*rx=page_alloc(a.pid),*rw=page_alloc(a.pid),*ro=page_alloc(a.pid),*rw2=page_alloc(a.pid);CHECK(rx&&rw&&ro&&rw2);
 CHECK(!space_map(&a,SF_BASE,(uint64_t)rx,5));
 CHECK(!space_map(&a,SF_BASE+4096,(uint64_t)ro,1));
 CHECK(!space_map(&a,SF_HEAP,(uint64_t)rw,3));
 CHECK(!space_map(&a,SF_HEAP+4096,(uint64_t)rw2,3));
 CHECK(sf_compat_register(&a,SF_BASE,SF_HEAP,0)==SF_OK);
 CHECK(tasks[0].compat_handler==SF_BASE&&tasks[0].compat_frame==SF_HEAP);
 CHECK(sf_compat_register(&a,SF_BASE,SF_HEAP,0)==SF_BUSY);
 reset(&a);
 CHECK(sf_compat_register(&a,SF_HEAP,SF_HEAP,0)==SF_DENIED);      /* 入口在 RW/NX 页 */
 CHECK(sf_compat_register(&a,SF_BASE+4096,SF_HEAP,0)==SF_DENIED); /* 入口只读不可执行 */
 CHECK(sf_compat_register(&a,SF_BASE,SF_BASE+4096,0)==SF_DENIED); /* 帧页不可写 */
 CHECK(sf_compat_register(&a,SF_BASE,SF_HEAP+1,0)==SF_INVALID);    /* 帧页未对齐 */
 CHECK(sf_compat_register(&a,SF_BASE,SF_HEAP,1)==SF_INVALID);      /* flags 必须为零 */
 CHECK(sf_compat_register(&a,SF_BASE,SF_HEAP+8192,0)==SF_DENIED);  /* 帧页未映射 */
 CHECK(!tasks[0].compat_handler);
 CHECK(sf_compat_register(&a,SF_BASE,SF_HEAP,0)==SF_OK);
 /* 线程不持有描述符，注册必须拒绝，避免共享帧页。 */
 Task th=a;th.tid=2;th.main=0;th.thread_slot=1;
 CHECK(sf_compat_register(&th,SF_BASE,SF_HEAP,0)==SF_DENIED);
 /* 另一进程的帧页地址在本进程未映射，必须拒绝。 */
 Task b={0};b.pid=22;b.main=1;b.main_slot=1;b.page_limit=SF_PAGE_LIMIT_MAX;tasks[1].pid=22;tasks[1].main=1;tasks[1].main_slot=1;tasks[1].page_limit=SF_PAGE_LIMIT_MAX;b.cr3=space_new(b.pid);CHECK(b.cr3);
 uint8_t*foreign=page_alloc(b.pid);CHECK(foreign);CHECK(!space_map(&b,SF_HEAP+8192,(uint64_t)foreign,3));
 reset(&a);
 CHECK(sf_compat_register(&a,SF_BASE,SF_HEAP+8192,0)==SF_DENIED);
 Frame f={0};f.cs=0x1b;f.ss=0x23;f.rip=SF_BASE;f.rsp=SF_STACK-8;
 CHECK(sf_compat_frame_valid(&f));
 f.cs=0x08;CHECK(!sf_compat_frame_valid(&f));f.cs=0x1b;
 f.ss=0x10;CHECK(!sf_compat_frame_valid(&f));f.ss=0x23;
 f.rip=0xffffffff80000000ull;CHECK(!sf_compat_frame_valid(&f));f.rip=SF_BASE;
 f.rsp=0x0000800000000000ull;CHECK(!sf_compat_frame_valid(&f));f.rsp=SF_STACK-8;
 CHECK(!sf_compat_frame_valid(0));
 pages_free(a.pid);pages_free(b.pid);printf("Compat tests passed: %u assertions\n",checks);return 0;
}
