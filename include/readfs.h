#ifndef READFS_H
#define READFS_H
#include "package.h"
#define SF_READFS_MAX_ENTRIES 16u
#define SF_READFS_FILE_MAX (128u*1024u)
#define SF_READFS_IMAGE_MAX (8u*1024u*1024u)
typedef int (*SfSectorRead)(void*,uint64_t,uint8_t[512]);
typedef struct {char path[64];uint64_t offset,bytes;uint8_t hash[32];} SfReadFile;
typedef struct {SfSectorRead read;void*context;uint64_t capacity;uint32_t count,mounted;SfReadFile files[SF_READFS_MAX_ENTRIES];} SfReadFs;
int sf_readfs_mount(SfReadFs*,SfSectorRead,void*,uint64_t);
int sf_readfs_find(const SfReadFs*,const char*);
long sf_readfs_read(SfReadFs*,unsigned,uint64_t,void*,size_t);
long sf_readfs_load(SfReadFs*,unsigned,void*,size_t);
int sf_readfs_path(const char*);
#endif
