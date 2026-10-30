#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/types.h>
#include <stdint.h>

/* Signal numbers — aligned with kernel process.h */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22

/* Signal handler special values */
#define SIG_DFL  ((void (*)(int))0)
#define SIG_IGN  ((void (*)(int))1)
#define SIG_ERR  ((void (*)(int))-1)

/* sigaction structure — must match kernel linux_sigaction_t */
struct sigaction {
    void     (*sa_handler)(int);
    uint64_t  sa_flags;
    uint64_t  sa_mask;
    void    (*sa_restorer)(void);
};

/* sa_flags */
#define SA_NOCLDSTOP  0x00000001
#define SA_NOCLDWAIT  0x00000002
#define SA_SIGINFO    0x00000004
#define SA_RESTART    0x10000000
#define SA_RESTORER   0x04000000

typedef void (*sighandler_t)(int);

int kill(pid_t pid, int sig);
int raise(int sig);
sighandler_t signal(int sig, sighandler_t handler);
int sigaction(int sig, const struct sigaction *act, struct sigaction *oact);

#endif /* _SIGNAL_H */
