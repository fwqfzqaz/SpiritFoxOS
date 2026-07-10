#ifndef _FCNTL_H
#define _FCNTL_H

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT   0x40
#define O_EXCL    0x80
#define O_NOCTTY  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC 0x80000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define AT_FDCWD -100

#endif
