/*
 * SpiritFoxOS Signal Delivery
 *
 * Checks and delivers pending signals to user-mode processes.
 * Extracted from process.c for modularity.
 */

#include "process.h"
#include "hal.h"
#include "memory.h"
#include "string.h"

extern process_t *current;

/* ========================================================================
 * process_signal_deliver() – check and deliver pending signals
 *
 * Called before returning to user mode.  If a signal is pending and
 * not blocked, set up the user stack to jump to the signal handler.
 * After the handler runs, it calls sigreturn to resume the original context.
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

    /* Clear the pending bit */
    current->pending_signals &= ~(1ULL << (sig - 1));

    uint64_t handler = current->signal_handlers[sig - 1];

    /* Default action: for SIGKILL/SIGSTOP terminate/stop,
     * for others ignore if handler is 0 */
    if (handler == 0) {
        if (sig == SIGKILL || sig == SIGSTOP) {
            process_exit(128 + sig);
            __builtin_unreachable();
        }
        /* Default action for most signals: ignore */
        return;
    }

    /* If no trap frame, we can't deliver (kernel thread) */
    if (!current->trap_frame)
        return;

    /* Set up signal handler invocation on the user stack.
     * We push a signal frame that the signal handler can return from
     * via sigreturn.  The frame layout on the user stack:
     *
     *   [trap_frame_t copy]  – saved context for sigreturn
     *   [signal number]      – first arg to handler
     *
     * The handler's return address points to a trampoline that
     * executes rt_sigreturn syscall.  For simplicity, we use
     * current->signal_restorer as the return address.
     */

    /* Read current trap frame from the kernel stack */
    trap_frame_t *tf = current->trap_frame;
    uint64_t user_sp = tf->rsp;

    /* Push a copy of the trap frame onto the user stack */
    user_sp -= sizeof(trap_frame_t);
    write_user_mem(current, user_sp, tf, sizeof(trap_frame_t));

    /* Push signal number */
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

    /* Modify the trap frame to invoke handler */
    tf->rip = handler;
    tf->rsp = user_sp;
    tf->rdi = (uint64_t)sig;
    tf->rax = 0;
}
