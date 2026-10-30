/* SpiritFoxOS libc - Directory operations (opendir/readdir/closedir)
 *
 * Uses the getdents64 syscall to read directory entries.
 * The kernel's getdents64 fills linux_dirent64 structures which
 * map directly to our struct dirent.
 */

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"

DIR *opendir(const char *name)
{
    int fd = open(name, O_RDONLY | 0x10000 /* O_DIRECTORY */);
    if (fd < 0)
        return NULL;

    DIR *dir = (DIR *)calloc(1, sizeof(DIR));
    if (!dir) {
        close(fd);
        return NULL;
    }
    dir->fd = fd;
    dir->buf_pos = 0;
    dir->buf_end = 0;
    return dir;
}

struct dirent *readdir(DIR *dirp)
{
    if (!dirp)
        return NULL;

    /* If buffer is exhausted, read more entries */
    if (dirp->buf_pos >= dirp->buf_end) {
        int64_t ret = sfk_syscall3(SYS_getdents64, dirp->fd,
                                    (int64_t)dirp->buf, sizeof(dirp->buf));
        if (ret <= 0) {
            if (ret < 0) sfk_errno = -(int)ret;
            return NULL;
        }
        dirp->buf_pos = 0;
        dirp->buf_end = (int)ret;
    }

    /* Get current dirent entry */
    struct dirent *d = (struct dirent *)(dirp->buf + dirp->buf_pos);
    dirp->buf_pos += d->d_reclen;
    return d;
}

int closedir(DIR *dirp)
{
    if (!dirp)
        return -1;
    int ret = close(dirp->fd);
    free(dirp);
    return ret;
}

void rewinddir(DIR *dirp)
{
    if (!dirp)
        return;
    lseek(dirp->fd, 0, SEEK_SET);
    dirp->buf_pos = 0;
    dirp->buf_end = 0;
}
