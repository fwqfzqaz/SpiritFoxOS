/* S4 线程夹具：TLS 逐线程独立、futex 互斥计数、clone 双返回、
 * 线程上限 EAGAIN、futex 超时与唤醒丢失负例。 */
#include "linux_raw.h"

#define THREAD_STACK (64u*1024u)
#define LIMIT_STACK (16u*1024u)
#define LIMIT_STACK_COUNT 32

typedef struct {
 int tid;              /* CLONE_CHILD_CLEARTID 在线程退出时清零 */
 int (*fn)(void*);
 void *arg;
 void *stack;
} LxThread;

static LxThread threads[LIMIT_STACK_COUNT];
static unsigned long tls_area[4][16];
static volatile int mutex_word;
static volatile int gate_open;
static volatile int gate_started;
static volatile int counter;

/* clone 的 C 包装：rdi=fn,rsi=arg,rdx=stack,rcx=tls,r8=tid,r9=flags。
 * 子线程在 rsp=stack 处弹出 fn/arg 并调用，随后 exit；父线程拿到 tid。 */
long lx_clone_start(int (*fn)(void*),void *arg,void *stack,void *tls,int *tid,unsigned long flags);
__asm__(
".section .text\n"
".global lx_clone_start\n"
"lx_clone_start:\n"
" mov %rdi,%r11\n"
" and $-16,%rdx\n"
" sub $16,%rdx\n"
" mov %rsi,8(%rdx)\n"
" mov %r11,0(%rdx)\n"
" mov %rdx,%rsi\n"
" mov %r8,%rdx\n"
" mov %r8,%r10\n"
" mov %rcx,%r8\n"
" mov %r9,%rdi\n"
" mov $56,%eax\n"
" syscall\n"
" test %eax,%eax\n"
" jnz 1f\n"
" xor %ebp,%ebp\n"
" pop %rax\n"
" pop %rdi\n"
" call *%rax\n"
" mov %eax,%edi\n"
" mov $60,%eax\n"
" syscall\n"
"1: ret\n");

static inline int cas(volatile int *p,int old,int value){
 int prev;__asm__ volatile("lock cmpxchgl %2,%1":"=a"(prev),"+m"(*p):"r"(value),"0"(old):"memory");return prev;
}
static inline int xchg(volatile int *p,int value){__asm__ volatile("xchgl %0,%1":"+r"(value),"+m"(*p)::"memory");return value;}
static inline void fs_write(long off,unsigned long value){__asm__ volatile("movq %0,%%fs:(%1)"::"r"(value),"r"(off):"memory");}
static inline unsigned long fs_read(long off){unsigned long value;__asm__ volatile("movq %%fs:(%1),%0":"=r"(value):"r"(off):"memory");return value;}

static void *lx_mmap(unsigned long bytes){
 long r=lx_raw(LX_SYS_MMAP,0,(long)bytes,LX_PROT_READ|LX_PROT_WRITE,LX_MAP_PRIVATE|LX_MAP_ANONYMOUS,-1,0);
 if(r<0){lx_puts("LINUX THREAD MMAP ERROR ");lx_putnum((unsigned long)-r);lx_puts("\n");return(void*)0;}
 return(void*)r;
}
static void lx_munmap(void *p,unsigned long bytes){if(p)lx_raw(LX_SYS_MUNMAP,(long)p,(long)bytes,0,0,0,0);}
static void lx_yield(void){lx_raw(LX_SYS_SCHED_YIELD,0,0,0,0,0,0);}

static long futex_wait(volatile int *addr,int value){
 return lx_raw(LX_SYS_FUTEX,(long)addr,LX_FUTEX_WAIT|LX_FUTEX_PRIVATE_FLAG,value,0,0,0);
}
static long futex_wait_timeout(volatile int *addr,int value,const LxTimespec *timeout){
 return lx_raw(LX_SYS_FUTEX,(long)addr,LX_FUTEX_WAIT|LX_FUTEX_PRIVATE_FLAG,value,(long)timeout,0,0);
}
static long futex_wake(volatile int *addr,int count){return lx_raw(LX_SYS_FUTEX,(long)addr,LX_FUTEX_WAKE|LX_FUTEX_PRIVATE_FLAG,count,0,0,0);}

static unsigned long now_ms(void){
 LxTimespec ts;
 if(lx_raw(LX_SYS_CLOCK_GETTIME,1,(long)&ts,0,0,0,0))return 0;
 return(unsigned long)ts.tv_sec*1000ul+(unsigned long)ts.tv_nsec/1000000ul;
}

static void mutex_lock(void){
 int c=0;
 if(!cas(&mutex_word,0,1))return;
 for(;;){
  if(c==2||xchg(&mutex_word,2))futex_wait(&mutex_word,2);
  c=0;
  if(!cas(&mutex_word,0,1))return;
 }
}
static void mutex_unlock(void){if(xchg(&mutex_word,0)==2)futex_wake(&mutex_word,1);}

static long thread_create_on_stack(LxThread *t,int (*fn)(void*),void *arg,void *tls,void *stack_top);
static long thread_create(LxThread *t,int (*fn)(void*),void *arg,void *tls){
 void *stack=lx_mmap(THREAD_STACK);
 if(!stack)return-LX_ENOMEM;
 long tid=thread_create_on_stack(t,fn,arg,tls,(char*)stack+THREAD_STACK);
 t->stack=stack;
 if(tid<0){lx_munmap(stack,THREAD_STACK);t->stack=0;}
 return tid;
}
/* The limit probe supplies pre-mapped guest stacks to avoid allocating one
 * Linux userspace stack per clone. Kernel-created native stacks remain 64 KiB. */
static long thread_create_on_stack(LxThread *t,int (*fn)(void*),void *arg,void *tls,void *stack_top){
 t->tid=0;t->fn=fn;t->arg=arg;t->stack=stack_top;
 unsigned long flags=LX_CLONE_VM|LX_CLONE_FS|LX_CLONE_FILES|LX_CLONE_SIGHAND|LX_CLONE_THREAD|LX_CLONE_SYSVSEM|LX_CLONE_CHILD_SETTID|LX_CLONE_CHILD_CLEARTID;
 if(tls)flags|=LX_CLONE_SETTLS;
 return lx_clone_start(fn,arg,stack_top,tls,&t->tid,flags);
}
static void thread_join(LxThread *t){
 while(t->tid)futex_wait(&t->tid,t->tid);
 lx_munmap(t->stack,THREAD_STACK);t->stack=0;
}
static void thread_join_on_arena(LxThread *t){
 while(t->tid)futex_wait(&t->tid,t->tid);
 t->stack=0;
}

static int tls_thread(void *p){
 int index=(int)(long)p;
 unsigned long base=(unsigned long)&tls_area[index][0];
 if(lx_raw(LX_SYS_ARCH_PRCTL,LX_ARCH_SET_FS,(long)base,0,0,0,0))return 1;
 fs_write(0,0x1000ul+(unsigned long)index);
 fs_write(8,0x2000ul+(unsigned long)index);
 for(int i=0;i<20;i++)lx_yield();
 if(fs_read(0)!=0x1000ul+(unsigned long)index||fs_read(8)!=0x2000ul+(unsigned long)index)return 2;
 return 0;
}

static int counter_thread(void *p){
 (void)p;
 for(int i=0;i<1000;i++){mutex_lock();counter++;mutex_unlock();}
 return 0;
}

static int gate_thread(void *p){
 (void)p;
 __atomic_fetch_add(&gate_started,1,__ATOMIC_RELEASE);
 while(!__atomic_load_n(&gate_open,__ATOMIC_ACQUIRE))futex_wait(&gate_open,0);
 return 0;
}

#define REQUIRE(x) do{if(!(x)){lx_puts("LINUX THREAD FAIL ");lx_putnum(__LINE__);lx_puts("\n");return 1;}}while(0)

static int thread_main(void){
 /* TLS：两个线程各自设置 %fs 基址，交替让出后仍读回自己的值。 */
 REQUIRE(lx_raw(LX_SYS_ARCH_PRCTL,LX_ARCH_SET_FS,(long)&tls_area[3][0],0,0,0,0)==0);
 fs_write(0,0x3000);fs_write(8,0x4000);
 LxThread tls_threads[2];
 REQUIRE(thread_create(&tls_threads[0],tls_thread, (void*)0,&tls_area[0][0])>0);
 REQUIRE(thread_create(&tls_threads[1],tls_thread, (void*)1,&tls_area[1][0])>0);
 thread_join(&tls_threads[0]);thread_join(&tls_threads[1]);
 REQUIRE(fs_read(0)==0x3000&&fs_read(8)==0x4000);
 REQUIRE(tls_area[0][0]==0x1000&&tls_area[1][0]==0x1001);
 REQUIRE(tls_area[0][1]==0x2000&&tls_area[1][1]==0x2001);
 lx_puts("LINUX THREAD TLS OK\n");
 /* 互斥：4 线程 × 1000 次，futex 阻塞/唤醒保护共享计数器。 */
 counter=0;
 LxThread workers[4];
 for(int i=0;i<4;i++)REQUIRE(thread_create(&workers[i],counter_thread,0,0)>0);
 for(int i=0;i<4;i++)thread_join(&workers[i]);
 REQUIRE(counter==4000);
 lx_puts("LINUX THREAD MUTEX OK\n");
 /* 上限：持续创建直到内核返回 EAGAIN，再统一放行并回收。 */
 gate_open=0;gate_started=0;
 /* The 16 KiB Linux stacks are only fixture call stacks. The native compat
  * thread stacks allocated by the kernel remain 64 KiB each. Pre-map all
  * slots, including the final clone expected to be rejected, before cloning. */
 void *stack_arena=lx_mmap(LIMIT_STACK*LIMIT_STACK_COUNT);
 REQUIRE(stack_arena!=0);
 int created=0;
 long limit_result=0;
 for(int i=0;i<LIMIT_STACK_COUNT;i++){
  void *stack_top=(char*)stack_arena+(i+1)*LIMIT_STACK;
  long r=thread_create_on_stack(&threads[i],gate_thread,0,0,stack_top);
  if(r<0){limit_result=r;break;}
  created++;
 }
 while(__atomic_load_n(&gate_started,__ATOMIC_ACQUIRE)<created)lx_yield();
 __atomic_store_n(&gate_open,1,__ATOMIC_RELEASE);
 futex_wake(&gate_open,created);
 for(int i=0;i<created;i++)thread_join_on_arena(&threads[i]);
 REQUIRE(lx_raw(LX_SYS_MUNMAP,(long)stack_arena,LIMIT_STACK*LIMIT_STACK_COUNT,0,0,0,0)==0);
 if(limit_result!=-LX_EAGAIN||created!=31){lx_puts("LINUX THREAD CREATE RESULT ");lx_putnum((unsigned long)(limit_result<0?-limit_result:0));lx_puts(" created=");lx_putnum((unsigned long)created);lx_puts("\n");}
 REQUIRE(limit_result==-LX_EAGAIN);
 REQUIRE(created==31);
 lx_puts("LINUX THREAD LIMIT COUNT ");lx_putnum(created);lx_puts("\n");
 lx_puts("LINUX THREAD LIMIT OK\n");
 /* futex 负例：值不匹配立即 EAGAIN；无等待者 WAKE 返回 0；20ms 超时。 */
 volatile int word=1;
 REQUIRE(futex_wait(&word,0)==-LX_EAGAIN);
 REQUIRE(futex_wake(&word,1)==0);
 LxTimespec timeout={0,20000000};
 unsigned long start=now_ms();
 long r=futex_wait_timeout(&word,1,&timeout);
 unsigned long elapsed=now_ms()-start;
 REQUIRE(r==-LX_ETIMEDOUT&&elapsed>=20&&elapsed<200);
 lx_puts("LINUX THREAD FUTEX OK\n");
 lx_puts("LINUX THREAD OK\n");
 return 0;
}

long sf_tool_main(long argc,char **argv){
 (void)argc;(void)argv;
 if(thread_main())return 1;
 lx_puts("LINUX THREAD TESTS PASSED\n");
 return 0;
}
