#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

/* ========================================================================
 * ioctl 宏定义 (Linux asm-generic/ioctl.h 兼容)
 *
 * 供用户态程序和系统头文件 (linux/drm.h, linux/input.h) 使用。
 * ======================================================================== */

#define _IOC_NRBITS     8
#define _IOC_TYPEBITS   8
#define _IOC_SIZEBITS   14
#define _IOC_DIRBITS    2

#define _IOC_NRMASK     ((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK   ((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK   ((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK    ((1 << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT  (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT   (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE       0U
#define _IOC_WRITE      1U
#define _IOC_READ       2U

#define _IOC(dir, type, nr, size) \
    (((dir)  << _IOC_DIRSHIFT) | \
     ((type) << _IOC_TYPESHIFT) | \
     ((nr)   << _IOC_NRSHIFT) | \
     ((size) << _IOC_SIZESHIFT))

#define _IO(type, nr)        _IOC(_IOC_NONE,    (type), (nr), 0)
#define _IOW(type, nr, size) _IOC(_IOC_WRITE,   (type), (nr), sizeof(size))
#define _IOR(type, nr, size) _IOC(_IOC_READ,    (type), (nr), sizeof(size))
#define _IOWR(type, nr, size) _IOC(_IOC_READ|_IOC_WRITE, (type), (nr), sizeof(size))

#define _IOC_DIR(cmd)    (((cmd) >> _IOC_DIRSHIFT)  & _IOC_DIRMASK)
#define _IOC_TYPE(cmd)   (((cmd) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(cmd)     (((cmd) >> _IOC_NRSHIFT)   & _IOC_NRMASK)
#define _IOC_SIZE(cmd)   (((cmd) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

int ioctl(int fd, unsigned long request, ...);

#endif /* _SYS_IOCTL_H */
