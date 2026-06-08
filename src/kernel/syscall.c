#include "syscall.h"
#include "idt.h"
#include "log.h"
#include "../include/io.h"

/* Syscall handler table */
static syscall_handler_t syscall_handlers[SYSCALL_MAX];

/* Default syscall handler - returns -1 (ENOSYS) */
static int64_t syscall_default(uint64_t arg0, uint64_t arg1,
                               uint64_t arg2, uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return -1;
}

/* Interrupt 0x80 handler - dispatch system calls */
static void syscall_interrupt_handler(struct interrupt_frame *frame) {
    /* Syscall convention:
     * RAX = syscall number
     * RDI = arg0, RSI = arg1, RDX = arg2, RCX = arg3
     * Return value in RAX
     */
    uint64_t syscall_num = frame->rax;
    uint64_t arg0 = frame->rdi;
    uint64_t arg1 = frame->rsi;
    uint64_t arg2 = frame->rdx;
    uint64_t arg3 = frame->rcx;

    if (syscall_num >= SYSCALL_MAX || !syscall_handlers[syscall_num]) {
        frame->rax = (uint64_t)-1;
        return;
    }

    int64_t result = syscall_handlers[syscall_num](arg0, arg1, arg2, arg3);
    frame->rax = (uint64_t)result;
}

/* Basic syscall implementations */
static int64_t sys_write(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg1; (void)arg2; (void)arg3;
    /* arg0 = fd (0=stdout), buffer pointed by arg1, length in arg2 */
    /* Simplified: just return success */
    (void)arg0;
    return 0;
}

static int64_t sys_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg1; (void)arg2; (void)arg3;
    /* arg0 = exit code */
    LOG_I("syscall", "Process exit with code %u", (uint32_t)arg0);
    return 0;
}

static int64_t sys_getpid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return 0; /* Kernel shell always PID 0 for now */
}

static int64_t sys_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return 0;
}

static int64_t sys_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg1; (void)arg2; (void)arg3;
    /* arg0 = milliseconds */
    extern volatile uint64_t timer_ticks;
    uint64_t target = timer_ticks + (arg0 / 10);
    while (timer_ticks < target) hlt();
    return 0;
}

void syscall_init(void) {
    /* Initialize all handlers to default */
    for (int i = 0; i < SYSCALL_MAX; i++) {
        syscall_handlers[i] = syscall_default;
    }

    /* Register basic syscall handlers */
    syscall_handlers[SYS_WRITE]   = sys_write;
    syscall_handlers[SYS_EXIT]    = sys_exit;
    syscall_handlers[SYS_GETPID]  = sys_getpid;
    syscall_handlers[SYS_YIELD]   = sys_yield;
    syscall_handlers[SYS_SLEEP]   = sys_sleep;

    /* Register interrupt 0x80 handler */
    idt_register_handler(0x80, syscall_interrupt_handler);

    LOG_I("syscall", "System call interface initialized (int 0x80, %d syscalls)", SYSCALL_MAX);
}

void syscall_register(uint64_t syscall_num, syscall_handler_t handler) {
    if (syscall_num >= SYSCALL_MAX) return;
    syscall_handlers[syscall_num] = handler;
}
