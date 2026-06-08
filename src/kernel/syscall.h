#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* System call numbers */
#define SYS_READ     0
#define SYS_WRITE    1
#define SYS_OPEN     2
#define SYS_CLOSE    3
#define SYS_EXIT     4
#define SYS_GETPID   5
#define SYS_YIELD    6
#define SYS_SLEEP    7
#define SYS_PUTS     8
#define SYS_GETCHAR  9
#define SYS_MMAP     10
#define SYS_MUNMAP   11
#define SYS_INFO     12

/* Maximum number of syscall handlers */
#define SYSCALL_MAX  32

/* Syscall handler type: takes syscall number + up to 4 args, returns result */
typedef int64_t (*syscall_handler_t)(uint64_t arg0, uint64_t arg1,
                                     uint64_t arg2, uint64_t arg3);

/* Initialize the system call interface (register int 0x80 handler) */
void syscall_init(void);

/* Register a handler for a specific syscall number */
void syscall_register(uint64_t syscall_num, syscall_handler_t handler);

#endif /* SYSCALL_H */
