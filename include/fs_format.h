#ifndef FS_FORMAT_H
#define FS_FORMAT_H
#include "readfs.h"
/* Generic on-disk filesystem registry. SFRFS1 is the original fixed profile;
 * SFRFS2 is the nested-tree profile with larger limits and empty-file support. */
#define SF_FS_FORMAT_AUTO 0
#define SF_FS_FORMAT_SFRFS1 1
#define SF_FS_FORMAT_SFRFS2 2
#define SF_READFS2_MAX_ENTRIES 48u
#define SF_READFS2_FILE_MAX (1024u*1024u)
#define SF_READFS2_IMAGE_MAX (32u*1024u*1024u)
int sf_fs_probe(SfSectorRead,void*,uint64_t,uint64_t*);
int sf_fs_mount_auto(SfReadFs*,SfSectorRead,void*,uint64_t,int,int*);
#endif
