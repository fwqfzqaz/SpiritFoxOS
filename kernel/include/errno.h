#ifndef ERRNO_H
#define ERRNO_H

/* Linux-compatible error codes */
#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define ENXIO        6
#define EAGAIN      11
#define ECHILD      10
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define ENOTBLK     15
#define EBUSY       16
#define EBADF       9
#define EEXIST      17
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENFILE      23
#define EMFILE      24
#define ENOTTY      25
#define ERANGE      34
#define ENOSPC      28
#define ESPIPE      29
#define EROFS       30
#define ENOSYS      38
#define ENAMETOOLONG 63
#define ENOTSOCK    88
#define EOPNOTSUPP  95
#define EAFNOSUPPORT 97
#define ECONNREFUSED 111

#endif /* ERRNO_H */
