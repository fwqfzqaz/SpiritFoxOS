#include <stdio.h>
#include "../../kernel/kernel.h"
/* 匿名映射与共享内存机制：窗口边界、重叠拒绝、配额回滚、读写权限、
 * 授权规则、关闭回收与进程死亡级联。 */
Task tasks[SF_MAX_THREADS];
int cpu_currents[SF_MAX_CPUS];
uint64_t ticks;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("SHM FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int process(unsigned slot,uint32_t pid){
 Task*t=&tasks[slot];t->pid=pid;t->state=T_READY;t->main=1;t->main_slot=slot;t->tid=1;t->thread_count=1;
 t->thread_slots=1;t->thread_limit=SF_THREAD_LIMIT_MAX;t->cap_limit=SF_CAP_LIMIT_MAX;t->page_limit=SF_PAGE_LIMIT_MAX;
 t->cr3=space_new(pid);return t->cr3?0:1;
}
static long call(unsigned nr,uint64_t a,uint64_t b){return shm_syscall(nr,a,b,0);}
int main(void){
 CHECK(process(0,1)==0&&process(1,2)==0);cpu_currents[0]=0;
 /* 匿名映射：窗口内自底分配、零填充、重叠与越界拒绝、munmap 后可重分配。 */
 long a=space_map_anon(&tasks[0],0,8192);CHECK(a==(long)SF_HEAP);
 uint8_t*p=user_phys(&tasks[0],(uint64_t)a),*p2=user_phys(&tasks[0],(uint64_t)a+4096);
 CHECK(user_range(&tasks[0],(uint64_t)a,8192,1)&&p&&p2&&p[0]==0&&p2[4096-1]==0);
 CHECK(space_map_anon(&tasks[0],SF_HEAP,4096)==SF_INVALID);
 long b=space_map_anon(&tasks[0],0,4096);CHECK(b==(long)(SF_HEAP+8192));
 CHECK(space_map_anon(&tasks[0],SF_HEAP+SF_HEAP_BYTES-4096,8192)==SF_INVALID);
 CHECK(space_map_anon(&tasks[0],SF_HEAP+SF_HEAP_BYTES+4096,4096)==SF_INVALID);
 CHECK(space_map_anon(&tasks[0],SF_BASE,4096)==SF_INVALID);
 CHECK(space_unmap_range(&tasks[0],(uint64_t)a,8192)==SF_OK&&!user_range(&tasks[0],(uint64_t)a,8192,0));
 CHECK(space_map_anon(&tasks[0],0,4096)==(long)SF_HEAP);
 /* 配额失败必须完整回滚，不留部分映射。 */
 uint32_t used=tasks[0].page_count;tasks[0].page_limit=used;
 CHECK(space_map_anon(&tasks[0],0,4096)==SF_NO_SPACE&&tasks[0].page_count==used);
 tasks[0].page_limit=SF_PAGE_LIMIT_MAX;
 /* 共享内存：创建者可读写，被授权者只读；非 owner 不能授权或关闭。 */
 long id=call(SYS_SHM_CREATE,4096,0);CHECK(id==1);
 long base=call(SYS_SHM_MAP,id,0);CHECK(base==(long)SF_SHM);
 uint8_t*sp=user_phys(&tasks[0],(uint64_t)base);CHECK(sp);sp[0]=0x42;
 cpu_currents[0]=1;CHECK(call(SYS_SHM_MAP,id,0)==SF_DENIED);CHECK(call(SYS_SHM_GRANT,id,2)==SF_DENIED);
 cpu_currents[0]=0;CHECK(call(SYS_SHM_GRANT,id,2)==SF_OK);
 cpu_currents[0]=1;long gb=call(SYS_SHM_MAP,id,0);uint8_t*gp=user_phys(&tasks[1],(uint64_t)gb);
 CHECK(gb==(long)SF_SHM&&gp&&gp[0]==0x42);
 CHECK(!user_range(&tasks[1],SF_SHM,4096,1)&&user_range(&tasks[1],SF_SHM,4096,0));
 CHECK(call(SYS_SHM_CLOSE,id,0)==SF_DENIED);
 cpu_currents[0]=0;uint32_t before=tasks[0].page_count;
 CHECK(call(SYS_SHM_CLOSE,id,0)==SF_OK);
 CHECK(!user_range(&tasks[0],SF_SHM,4096,0)&&!user_range(&tasks[1],SF_SHM,4096,0)&&tasks[0].page_count==before-1);
 CHECK(call(SYS_SHM_MAP,id,0)==SF_INVALID);
 /* owner 死亡：撤销所有引用并归还页。 */
 id=call(SYS_SHM_CREATE,8192,0);CHECK(id==1);
 CHECK(call(SYS_SHM_MAP,id,0)==(long)SF_SHM);CHECK(call(SYS_SHM_GRANT,id,2)==SF_OK);
 cpu_currents[0]=1;CHECK(call(SYS_SHM_MAP,id,0)==(long)SF_SHM);
 uint32_t at_death=tasks[0].page_count;
 shm_release_pid(1);
 CHECK(!user_range(&tasks[1],SF_SHM,8192,0)&&tasks[0].page_count==at_death-2);
 CHECK(call(SYS_SHM_MAP,id,0)==SF_INVALID);
 /* 被授权者死亡：只撤销其映射与授权，owner 映射与页保持。 */
 cpu_currents[0]=0;id=call(SYS_SHM_CREATE,4096,0);CHECK(id==1);
 CHECK(call(SYS_SHM_MAP,id,0)==(long)SF_SHM);CHECK(call(SYS_SHM_GRANT,id,2)==SF_OK);
 cpu_currents[0]=1;CHECK(call(SYS_SHM_MAP,id,0)==(long)SF_SHM);
 shm_release_pid(2);
 CHECK(!user_range(&tasks[1],SF_SHM,4096,0));
 cpu_currents[0]=0;CHECK(user_range(&tasks[0],SF_SHM,4096,1)&&call(SYS_SHM_CLOSE,id,0)==SF_OK);
 printf("SHM tests passed: %u assertions\n",checks);return 0;
}
