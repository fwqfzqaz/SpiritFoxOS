/*
 * SpiritFoxOS 信号投递
 *
 * 检查并向用户态进程投递待处理信号。
 * 从 process.c 中提取以实现模块化。
 */

#include "process.h"
#include "hal.h"
#include "memory.h"
#include "string.h"

extern process_t *current;

/* ========================================================================
 * process_signal_deliver() – 检查并投递待处理信号
 *
 * 在返回用户态之前调用。如果有待处理且未被阻塞的信号，
 * 设置用户栈以跳转到信号处理函数。
 * 处理函数运行后，调用 sigreturn 恢复原始上下文。
 * ======================================================================== */

void process_signal_deliver(void)
{
    if (!current)
        return;

    /* Find the first pending, unblocked signal */
    uint64_t pending = current->pending_signals & ~current->signal_mask;
    if (pending == 0)
        return;

    int sig = 0;
    for (int i = 0; i < MAX_SIGNAL; i++) {
        if (pending & (1ULL << i)) {
            sig = i + 1;
            break;
        }
    }
    if (sig == 0)
        return;

    /* 清除待处理位 */
    current->pending_signals &= ~(1ULL << (sig - 1));

    uint64_t handler = current->signal_handlers[sig - 1];

    /* 默认动作：对于 SIGKILL/SIGSTOP 终止/停止，
     * 对于其他信号，如果处理函数为 0 则忽略 */
    if (handler == 0) {
        if (sig == SIGKILL || sig == SIGSTOP) {
            process_exit(128 + sig);
            __builtin_unreachable();
        }
        /* Default action for most signals: ignore */
        return;
    }

    /* 如果没有 trap frame，无法投递（内核线程） */
    if (!current->trap_frame)
        return;

    /* 在用户栈上设置信号处理函数调用。
     * 我们压入一个信号帧，信号处理函数可以通过
     * sigreturn 从中返回。用户栈上的帧布局：
     *
     *   [trap_frame_t 副本]  – sigreturn 的已保存上下文
     *   [信号编号]           – 处理函数的第一个参数
     *
     * 处理函数的返回地址指向一个跳板，
     * 执行 rt_sigreturn 系统调用。为简化起见，我们
     * 使用 current->signal_restorer 作为返回地址。
     */

    /* 从内核栈读取当前 trap frame */
    trap_frame_t *tf = current->trap_frame;
    uint64_t user_sp = tf->rsp;

    /* 将 trap frame 副本压入用户栈 */
    user_sp -= sizeof(trap_frame_t);
    write_user_mem(current, user_sp, tf, sizeof(trap_frame_t));

    /* 压入信号编号 */
    user_sp -= 8;
    uint64_t sig64 = (uint64_t)sig;
    write_user_mem(current, user_sp, &sig64, 8);

    /* Set up the trap frame to invoke the signal handler:
     * rip = handler address
     * rsp = user_sp (pointing to sig number, then saved frame)
     * rdi = signal number (first arg)
     * The return address for the handler should be signal_restorer,
     * which calls rt_sigreturn.  We push it on the user stack.
     */
    user_sp -= 8;
    uint64_t ret_addr = current->signal_restorer ? current->signal_restorer : 0;
    write_user_mem(current, user_sp, &ret_addr, 8);

    /* 修改 trap frame 以调用处理函数 */
    tf->rip = handler;
    tf->rsp = user_sp;
    tf->rdi = (uint64_t)sig;
    tf->rax = 0;
}
