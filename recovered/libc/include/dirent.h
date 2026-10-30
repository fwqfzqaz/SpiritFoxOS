#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>
#include <stdint.h>

/* Linux x86_64 dirent64 structure — must match kernel getdents64 format */
struct dirent {
    uint64_t d_ino;           /* Inode number */
    int64_t  d_off;           /* Offset to next dirent */
    uint16_t d_reclen;        /* Length of this dirent */
    uint8_t  d_type;          /* Type of file */
    char     d_name[256];     /* Filename (null-terminated) */
};

/* d_type values */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

/* Directory stream */
typedef struct {
    int fd;                        /* Directory file descriptor */
    char buf[1024];                /* Internal buffer for getdents64 */
    int buf_pos;                   /* Current position in buffer */
    int buf_end;                   /* End of valid data in buffer */
} DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
void rewinddir(DIR *dirp);

#endif /* _DIRENT_H */
