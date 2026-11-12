#include "kernel.h"
/* 共享内存机制：8 个对象槽、固定虚拟基址、页由创建者 PID 记账。
 * 内核只提供 create/map/grant/close；谁在何时共享由 ring-3 策略决定。
 * 授权收件人只有只读映射，创建者拥有读写映射。 */
typedef struct {
 uint32_t used,owner,slot,count;
 uint32_t grants[SF_SHM_SLOTS],grant_count;
 uint32_t mapped[SF_SHM_SLOTS],mapped_count;
 uint64_t phys[SF_SHM_SLOT_BYTES/4096];
} Shm;
static Shm shms[SF_SHM_SLOTS];
static uint64_t shm_base(const Shm*s){return SF_SHM+(uint64_t)s->slot*SF_SHM_SLOT_BYTES;}
/* 按 PID 找存活主任务（含正在终结但尚未归还页表的状态）。 */
static Task *shm_task(uint32_t pid){
 if(!pid)return 0;
 for(unsigned i=0;i<SF_MAX_THREADS;i++){Task*t=&tasks[i];if(t->main&&t->pid==pid&&t->cr3&&t->state!=T_EMPTY)return t;}
 return 0;
}
static int shm_has(const uint32_t*list,uint32_t n,uint32_t pid){for(uint32_t i=0;i<n;i++)if(list[i]==pid)return 1;return 0;}
static void shm_remove(uint32_t*list,uint32_t*n,uint32_t pid){
 for(uint32_t i=0;i<*n;i++)if(list[i]==pid){list[i]=list[--*n];return;}
}
static void shm_mark(Shm*s,uint32_t pid){if(!shm_has(s->mapped,s->mapped_count,pid)&&s->mapped_count<SF_SHM_SLOTS)s->mapped[s->mapped_count++]=pid;}
/* 销毁：撤销所有已记录映射，再兜底释放未被映射归还的页。 */
static void shm_destroy(Shm*s){
 uint64_t bytes=(uint64_t)s->count*4096,base=shm_base(s);
 for(uint32_t i=0;i<s->mapped_count;i++){
  Task*t=shm_task(s->mapped[i]);
  if(t)space_unmap_range(t,base,bytes);
 }
 for(uint32_t i=0;i<s->count;i++)page_free_phys(s->owner,s->phys[i]);
 memset(s,0,sizeof(*s));
}
/* 创建者可读写；被授权者只读。固定基址，重复映射幂等。 */
static long shm_map(Shm*s,Task*t,uint32_t pid){
 int owner=s->owner==pid;
 if(!owner&&!shm_has(s->grants,s->grant_count,pid))return SF_DENIED;
 uint64_t base=shm_base(s);
 for(uint32_t i=0;i<s->count;i++){
  void*cur=user_phys(t,base+(uint64_t)i*4096);
  if(cur){if((uint64_t)cur!=s->phys[i])return SF_INVALID;continue;}
  int r=space_map(t,base+(uint64_t)i*4096,s->phys[i],owner?3:1);
  if(r)return r;
 }
 shm_mark(s,pid);return(long)base;
}
long shm_syscall(uint64_t nr,uint64_t a,uint64_t b,uint64_t c){
 (void)c;Task*t=task_main(&tasks[current]);if(!t)return SF_INVALID;uint32_t pid=t->pid;
 if(nr==SYS_SHM_CREATE){
  uint64_t bytes=a;
  if(!bytes||(bytes&4095)||bytes>SF_SHM_SLOT_BYTES)return SF_INVALID;
  int slot=-1;for(unsigned i=0;i<SF_SHM_SLOTS;i++)if(!shms[i].used){slot=(int)i;break;}
  if(slot<0)return SF_NO_SPACE;
  Shm*s=&shms[slot];uint32_t n=(uint32_t)(bytes/4096);
  for(uint32_t i=0;i<n;i++){
   void*p=page_alloc(pid);
   if(!p){for(uint32_t j=0;j<i;j++)page_free_phys(pid,s->phys[j]);return SF_NO_SPACE;}
   s->phys[i]=(uint64_t)p;
  }
  s->used=1;s->owner=pid;s->slot=(uint32_t)slot;s->count=n;return slot+1;
 }
 uint64_t id=a;
 if(!id||id>SF_SHM_SLOTS)return SF_INVALID;
 Shm*s=&shms[id-1];if(!s->used)return SF_INVALID;
 if(nr==SYS_SHM_MAP)return shm_map(s,t,pid);
 if(nr==SYS_SHM_GRANT){
  uint32_t target=(uint32_t)b;
  if(s->owner!=pid&&pid!=1)return SF_DENIED;
  if(!shm_task(target))return SF_INVALID;
  if(shm_has(s->grants,s->grant_count,target))return SF_OK;
  if(s->grant_count>=SF_SHM_SLOTS)return SF_NO_SPACE;
  s->grants[s->grant_count++]=target;return SF_OK;
 }
 if(nr==SYS_SHM_CLOSE){if(s->owner!=pid)return SF_DENIED;shm_destroy(s);return SF_OK;}
 return SF_INVALID;
}
/* 进程终结：owner 死则销毁全部映射与页；被授权者死则撤销其授权与映射。 */
void shm_release_pid(uint32_t pid){
 if(!pid)return;
 for(unsigned i=0;i<SF_SHM_SLOTS;i++){
  Shm*s=&shms[i];if(!s->used)continue;
  if(s->owner==pid){shm_destroy(s);continue;}
  if(!shm_has(s->grants,s->grant_count,pid)&&!shm_has(s->mapped,s->mapped_count,pid))continue;
  if(shm_has(s->mapped,s->mapped_count,pid)){
   Task*t=shm_task(pid);
   if(t)space_unmap_range(t,shm_base(s),(uint64_t)s->count*4096);
  }
  shm_remove(s->mapped,&s->mapped_count,pid);
  shm_remove(s->grants,&s->grant_count,pid);
 }
}
