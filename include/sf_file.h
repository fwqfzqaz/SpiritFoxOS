#ifndef SF_FILE_H
#define SF_FILE_H
#include "sf.h"
enum {SF_OPEN_READ=1,SF_OPEN_WRITE=2,SF_OPEN_NONBLOCK=4,SF_OPEN_DIRECTORY=8};
enum {SF_TYPE_DIRECTORY=1,SF_TYPE_FILE,SF_TYPE_PIPE,SF_TYPE_CONSOLE,SF_TYPE_DISK};
/* One outstanding call per private reply endpoint; single-thread native ABI. */
typedef struct {uint32_t manager,reply,timeout_ms;int64_t error;} SfFileClient;
int sf_file_client_init(SfFileClient*,uint64_t manager,uint64_t reply,uint32_t timeout_ms);
int64_t sf_file_root(SfFileClient*);
int64_t sf_file_console(SfFileClient*);
int64_t sf_file_openat(SfFileClient*,uint64_t,const char*,uint32_t);
int64_t sf_file_dup(SfFileClient*,uint64_t);
int64_t sf_file_read(SfFileClient*,uint64_t,void*,size_t);
int64_t sf_file_write(SfFileClient*,uint64_t,const void*,size_t);
int64_t sf_file_enumerate(SfFileClient*,uint64_t,char*,size_t);
int64_t sf_file_stat(SfFileClient*,uint64_t,SfFileStat*);
int64_t sf_file_seek(SfFileClient*,uint64_t,uint64_t);
int64_t sf_file_poll(SfFileClient*,uint64_t);
int64_t sf_file_close(SfFileClient*,uint64_t);
int64_t sf_file_truncate(SfFileClient*,uint64_t,uint64_t);
int64_t sf_file_unlink(SfFileClient*,uint64_t,const char*);
int64_t sf_file_rmdir(SfFileClient*,uint64_t,const char*);
#endif
