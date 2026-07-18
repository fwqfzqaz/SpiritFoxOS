/* SpiritFoxOS libc - Signal handling */

#include <signal.h>
#include <unistd.h>
#include "internal.h"

int kill(pid_t pid, int sig)
{
    int64_t ret = sfk_syscall2(SYS_kill, pid, sig);
    SET_ERRNO(ret);
    return (int)ret;
}

int raise(int sig)
{
    return kill(getpid(), sig);
}

sighandler_t signal(int sig, sighandler_t handler)
{
    struct sigaction sa, old_sa;
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    sa.sa_mask = 0;
    sa.sa_restorer = NULL;
    if (sigaction(sig, &sa, &old_sa) < 0)
        return SIG_ERR;
    return old_sa.sa_handler;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
    int64_t ret = sfk_syscall4(SYS_rt_sigaction, sig,
                               (int64_t)act, (int64_t)oact, 8);
    SET_ERRNO(ret);
    return (int)ret;
}
