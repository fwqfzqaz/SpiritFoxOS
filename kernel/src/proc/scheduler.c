/*
 * SpiritFoxOS 调度器
 *
 * 带上下文切换的 Per-CPU 运行队列调度器。
 * 从 process.c 中提取以实现模块化。
 * 多核改造：每个 CPU 独立运行队列，通过 this_cpu() 访问。
 */

#include "process.h"
#include "gdt.h"
#include "hal.h"
#include "memory.h"
#include "timer.h"
#include "smp.h"
#include "string.h"
#include "serial.h"
#include "vga.h"

/* Process table defined in process.c */
extern process_t proc_table[];

/* 过渡期：保留全局 current 变量用于 process.c 等文件的直接访问 */
extern process_t *current;

/* Context switch primitive - defined in isr_stub.S */
extern void arch_switch_to(uint64_t *old_rsp_ptr, uint64_t new_rsp);
#define switch_to(old_rsp_ptr, new_rsp) arch_switch_to(old_rsp_ptr, new_rsp)

/* 汇编跳板 - 定义在 isr_stub.S 中 */
extern void kthread_trampoline_asm(void);

/* ========================================================================
 * 常量
 * ======================================================================== */

#define MAX_PROCS          256
#define DEFAULT_TIMESLICE  20      /* 1000Hz 下 20ms */
#define KERNEL_STACK_PAGES 2       /* 2 页 = 8KB 内核栈 */

/* ========================================================================
 * Process lookup
 * ======================================================================== */

process_t *process_get(int pid)
{
    if (pid < 0 || pid >= MAX_PROCS)
        return NULL;
    if (proc_table[pid].state == PROC_UNUSED)
        return NULL;
    return &proc_table[pid];
}

process_t *process_current(void)
{
    cpu_local_t *cpu = this_cpu();
    if (cpu && cpu->current_process)
        return cpu->current_process;
    /* 回退到全局变量（过渡期） */
    return current;
}

/* ========================================================================
 * 重新调度标志
 * ======================================================================== */

int need_reschedule_check(void)
{
    cpu_local_t *cpu = this_cpu();
    if (cpu->need_reschedule) {
        cpu->need_reschedule = 0;
        return 1;
    }
    return 0;
}

/* ========================================================================
 * Per-CPU 运行队列操作
 * ======================================================================== */

/* 入队：加锁操作本 CPU 运行队列 */
static void enqueue_process(process_t *proc)
{
    cpu_local_t *cpu = this_cpu();
    spinlock_acquire(&cpu->runqueue_lock);
    proc->state = PROC_READY;
    proc->next_in_queue = NULL;
    if (cpu->runqueue_tail) {
        cpu->runqueue_tail->next_in_queue = proc;
    } else {
        cpu->runqueue_head = proc;
    }
    cpu->runqueue_tail = proc;
    cpu->runqueue_count++;
    spinlock_release(&cpu->runqueue_lock);
}

/* 出队：加锁操作本 CPU 运行队列 */
static process_t *dequeue_process(void)
{
    cpu_local_t *cpu = this_cpu();
    spinlock_acquire(&cpu->runqueue_lock);
    process_t *proc = cpu->runqueue_head;
    if (proc) {
        cpu->runqueue_head = proc->next_in_queue;
        if (!cpu->runqueue_head)
            cpu->runqueue_tail = NULL;
        cpu->runqueue_count--;
        proc->next_in_queue = NULL;
    }
    spinlock_release(&cpu->runqueue_lock);
    return proc;
}

/* ========================================================================
 * process_yield()
 * ======================================================================== */

void process_yield(void)
{
    process_t *cur = process_current();
    if (cur)
        cur->state = PROC_READY;
    this_cpu()->need_reschedule = 1;
    scheduler_schedule();
}

/* ========================================================================
 * process_sleep() - 进程休眠
 * ======================================================================== */

void process_sleep(uint64_t ms)
{
    process_t *cur = process_current();
    if (!cur)
        return;
    cur->sleep_until = timer_get_ms() + ms;
    cur->state = PROC_BLOCKED;
    scheduler_schedule();
}

/* ========================================================================
 * 调度器时钟（从定时器中断调用）
 * ======================================================================== */

void scheduler_tick(void)
{
    cpu_local_t *cpu = this_cpu();
    if (!cpu) return;
    process_t *cur = cpu->current_process;
    if (!cur) return;

    cur->cpu_time++;
    cur->priority--;

    /* 唤醒时间已到的休眠进程 */
    uint64_t now = timer_get_ms();
    spinlock_acquire(&proc_table_lock);
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_BLOCKED &&
            proc_table[i].sleep_until > 0 &&
            proc_table[i].sleep_until <= now) {
            proc_table[i].state = PROC_READY;
            proc_table[i].sleep_until = 0;
            /* 将唤醒的进程加入当前 CPU 队列 */
            enqueue_process(&proc_table[i]);
        }
    }
    spinlock_release(&proc_table_lock);

    if (cur->priority <= 0)
        cpu->need_reschedule = 1;
}

/* ========================================================================
 * 调度器 - Per-CPU 运行队列调度
 * ======================================================================== */

__attribute__((noinline))
void scheduler_schedule(void)
{
    cpu_local_t *cpu = this_cpu();
    process_t *old = cpu->current_process;
    if (!old)
        return;

    process_t *new_p = dequeue_process();

    if (!new_p) {
        /* 本 CPU 队列为空，尝试从其他 CPU 窃取进程 */
        for (int i = 0; i < smp_get_cpu_count(); i++) {
            if (i == (int)cpu->index)
                continue;
            cpu_local_t *other = &cpu_locals[i];
            if (!other->online || other->runqueue_count <= 0)
                continue;

            spinlock_acquire(&other->runqueue_lock);
            process_t *stolen = other->runqueue_head;
            if (stolen) {
                other->runqueue_head = stolen->next_in_queue;
                if (!other->runqueue_head)
                    other->runqueue_tail = NULL;
                other->runqueue_count--;
                stolen->next_in_queue = NULL;
            }
            spinlock_release(&other->runqueue_lock);

            if (stolen) {
                new_p = stolen;
                printf("[SCHED] CPU%d stole PID%d from CPU%d\n",
                       cpu->index, new_p->pid, i);
                break;
            }
        }
    }

    if (!new_p) {
        /* 仍然没有找到——回退到全局 proc_table 扫描
         * （兼容未入队进程，如 PID0 idle） */
        int next = -1;
        spinlock_acquire(&proc_table_lock);
        for (int i = 1; i <= MAX_PROCS; i++) {
            int idx = (old->pid + i) % MAX_PROCS;
            if (proc_table[idx].state == PROC_READY) {
                next = idx;
                break;
            }
        }
        spinlock_release(&proc_table_lock);

        if (next < 0) {
            /* 没有其他 READY 进程，保持当前进程运行 */
            if (old->state == PROC_RUNNING || old->state == PROC_READY) {
                old->state = PROC_RUNNING;
                old->priority = DEFAULT_TIMESLICE;
            }
            cpu->need_reschedule = 0;
            return;
        }
        new_p = &proc_table[next];
    }

    /* 同一进程 – 无需切换 */
    if (old == new_p) {
        old->state = PROC_RUNNING;
        old->priority = DEFAULT_TIMESLICE;
        cpu->need_reschedule = 0;
        return;
    }

    if (old->state == PROC_RUNNING)
        old->state = PROC_READY;

    new_p->state = PROC_RUNNING;
    new_p->priority = DEFAULT_TIMESLICE;
    new_p->cpu_id = (int)cpu->index;
    cpu->need_reschedule = 0;

    /* Switch page tables if different */
    if (old->pml4 != new_p->pml4)
        hal_write_cr3(new_p->pml4);

    /* 更新 TSS rsp0 用于中断特权级转换 */
    if (new_p->kernel_stack) {
        gdt_set_tss_rsp0(cpu->index,
            (uint64_t)new_p->kernel_stack + (KERNEL_STACK_PAGES * PAGE_SIZE));
    } else {
        gdt_set_tss_rsp0(cpu->index, 0x800000);
    }

    /* 更新 syscall 暂存区的 gs:8（内核栈顶） */
    if (cpu->syscall_cpu_area) {
        uint64_t *area = (uint64_t *)cpu->syscall_cpu_area;
        area[1] = (new_p->kernel_stack)
            ? (uint64_t)new_p->kernel_stack + (KERNEL_STACK_PAGES * PAGE_SIZE)
            : 0x800000;
    }

    /* Sanity check: refuse to switch to a process with kernel_rsp == 0 */
    if (new_p->kernel_rsp == 0) {
        serial_puts("[SCHED] PANIC: PID");
        serial_put_dec((uint64_t)new_p->pid);
        serial_puts(" kernel_rsp=0, cannot switch!\n");
        if (old->state == PROC_READY)
            old->state = PROC_RUNNING;
        new_p->state = PROC_READY;
        cpu->need_reschedule = 0;
        return;
    }

    /* 上下文切换前的调试输出 */
    printf("[SCHED] PID%d -> PID%d krsp=%lx\n", old->pid, new_p->pid,
           (unsigned long)new_p->kernel_rsp);

    /* 同步全局 current（过渡期兼容） */
    current = new_p;
    cpu->current_process = new_p;
    switch_to(&old->kernel_rsp, new_p->kernel_rsp);

    /* 当本进程被重新调度时，执行到达此处。 */
}

/* ========================================================================
 * scheduler_start() - 进入空闲循环（永不返回）
 * ======================================================================== */

void scheduler_start(void)
{
    hal_enable_interrupts();
    while (1)
        hal_halt_no_cli();
    __builtin_unreachable();
}
