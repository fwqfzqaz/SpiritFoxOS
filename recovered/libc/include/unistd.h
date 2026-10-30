#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <stddef.h>

/* Standard file descriptors */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* File I/O */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
int dup(int fd);
int dup2(int oldfd, int newfd);
off_t lseek(int fd, off_t offset, int whence);
int pipe(int pipefd[2]);
int fcntl(int fd, int cmd, ...);

/* Directory / filesystem */
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int unlink(const char *path);
int rmdir(const char *path);
int mkdir(const char *path, mode_t mode);
int rename(const char *oldpath, const char *newpath);
int chmod(const char *path, mode_t mode);
int access(const char *pathname, int mode);
mode_t umask(mode_t mask);

/* Process management */
pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
gid_t getgid(void);
uid_t geteuid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
pid_t wait4(pid_t pid, int *status, int options, void *rusage);
pid_t waitpid(pid_t pid, int *status, int options);
int kill(pid_t pid, int sig);
int sched_yield(void);

/* Time / sleep */
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);

/* Miscellaneous */
int isatty(int fd);

/* _exit — direct syscall, no atexit */
void _exit(int status) __attribute__((noreturn));

#endif /* _UNISTD_H */
