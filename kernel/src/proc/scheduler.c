/*
 * SpiritFoxOS 调度器
 *
 * 带上下文切换的轮转进程调度器。
 * 从 process.c 中提取以实现模块化。
 */

#include "process.h"
#include "gdt.h"
#include "hal.h"
#include "memory.h"
#include "timer.h"
#include "string.h"
#include "serial.h"
#include "vga.h"

/* Processctable esdefinedain ble - defid in process.c */
extern process_t proc_table[];
extern process_t *current;
extern int need_reschedule;

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
 * PID 分配
 * ======================================================================== */

static int alloc_pid(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED)
            return i;
    }
    return -1;
}

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
    return current;
}

/* ========================================================================
 * 重新调度标志
 * ======================================================================== */

int need_reschedule_check(void)
{
    if (need_reschedule) {
        need_reschedule = 0;
        return 1;
    }
    return 0;
}

/* ========================================================================
 * process_yield()
 * ======================================================================== */

void process_yield(void)
{
    if (current)
        current->state = PROC_READY;
    need_reschedule = 1;
    scheduler_schedule();
}

/* ========================================================================
 * process_sleep() - 进程休眠
 * ======================================================================== */

void process_sleep(uint64_t ms)
{
    if (!current)
        return;
    current->sleep_until = timer_get_ms() + ms;
    current->state = PROC_BLOCKED;
    scheduler_schedule();
}

/* ========================================================================
 * 调度器时钟（从定时器中断调用）
 * ======================================================================== */

void scheduler_tick(void)
{
    if (!current)
        return;

    current->cpu_time++;
    current->priority--;

    /* 唤醒时间已到的休眠进程 */
    uint64_t now = timer_get_ms();
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_BLOCKED &&
            proc_table[i].sleep_until > 0 &&
            proc_table[i].sleep_until <= now) {
            proc_table[i].state = PROC_READY;
            proc_table[i].sleep_until = 0;
        }
    }

    if (current->priority <= 0)
        need_reschedule = 1;
}

/* ========================================================================
 * 调度器 - 轮转上下文切换
 * ======================================================================== */

__attribute__((noinline))
void scheduler_schedule(void)
{
    if (!current)
        return;

    /* Round-robin: find the next READY process after current */
    int start = current->pid;
    int next = -1;

    for (int i = 1; i <= MAX_PROCS; i++) {
        int idx = (start + i) % MAX_PROCS;
        if (proc_table[idx].state == PROC_READY) {
            next = idx;
            break;
        }
    }

    /* 没有其他 READY 进程 */
    if (next < 0) {
        if (current->state == PROC_RUNNING ||
            current->state == PROC_READY) {
            current->state = PROC_RUNNING;
            current->priority = DEFAULT_TIMESLICE;
            return;
        }
        /* 当前进程不可运行 – 查找任意进程 */
        for (int i = 0; i < MAX_PROCS; i++) {
            if (proc_table[i].state == PROC_READY) {
                next = i;
                break;
            }
        }
        if (next < 0) {
            /* Fall back to idle process */
            if (proc_table[0].state != PROC_UNUSED) {
                next = 0;
                proc_table[0].state = PROC_READY;
            } else {
                hal_halt();
            }
        }
    }

    process_t *old   = current;
    process_t *new_p = &proc_table[next];

    /* 同一进程 – 无需切换 */
    if (old == new_p) {
        old->state = PROC_RUNNING;
        old->priority = DEFAULT_TIMESLICE;
        need_reschedule = 0;
        return;
    }

    if (old->state == PROC_RUNNING)
        old->state = PROC_READY;

    new_p->state = PROC_RUNNING;
    new_p->priority = DEFAULT_TIMESLICE;
    need_reschedule = 0;

    /* Switch page tables if different */
    if (old->pml4 != new_p->pml4)
        hal_write_cr3(new_p->pml4);

    /* 更新 TSS rsp0 用于中断特权级转换 */
    if (new_p->kernel_stack) {
        tss.rsp0 = (uint64_t)new_p->kernel_stack +
                    (KERNEL_STACK_PAGES * PAGE_SIZE);
    } else {
        /* PID 0 使用启动栈；近似 rsp0 */
        uint64_t rsp_approx;
        __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp_approx));
        tss.rsp0 = rsp_approx + 512;
    }

    /* 上下文切换前的调试输出 */
    printf("[SCHED] PID%d -> PID%d krsp=%lx stack[0]=%lx stack[1]=%lx stack[2]=%lx stack[3]=%lx\n",
           old->pid, new_p->pid,
           (unsigned long)new_p->kernel_rsp,
           (unsigned long)((uint64_t *)new_p->kernel_rsp)[0],
           (unsigned long)((uint64_t *)new_p->kernel_rsp)[1],
           (unsigned long)((uint64_t *)new_p->kernel_rsp)[2],
           (unsigned long)((uint64_t *)new_p->kernel_rsp)[3]);

    /* 上下文切换 */
    current = new_p;
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
