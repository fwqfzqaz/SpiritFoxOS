#ifndef SF_NAMESPACE_CLIENT_H
#define SF_NAMESPACE_CLIENT_H
#include "namespace_handles.h"
#include "namespace_protocol.h"
/* Exchange returns the actual kernel receive sender (>0), or a transport
 * error (<0). It never returns message.status. It must bound its waits and
 * pump backend callbacks. The client correlates the copied reply itself. */
typedef int64_t(*SfNsExchange)(void*,SfMessage*);
typedef struct {
 SfNsHandleMap map;
 SfNsExchange exchange;
 void*context;
 void(*lost)(void*);
 uint64_t sequence;
 uint32_t peer,ready;
} SfNsClient;
/* Zero construct once. Connect preserves sequence and handle generations. */
int sf_nsc_connect(SfNsClient*,uint32_t,SfNsExchange,void*,void(*)(void*),uint32_t);
void sf_nsc_lost(SfNsClient*);
int sf_nsc_backends(SfNsClient*,uint32_t);
int sf_nsc_create(SfNsClient*,const char*,unsigned,int,uint32_t,uint32_t,unsigned);
int sf_nsc_assign(SfNsClient*,const char*,uint32_t,uint32_t,unsigned);
int sf_nsc_project(SfNsClient*,const char*,unsigned);
int sf_nsc_protect(SfNsClient*,const char*);
int sf_nsc_mount_check(SfNsClient*,const char*);
int sf_nsc_mount_publish(SfNsClient*,const SfMountEntry*,unsigned,unsigned);
int sf_nsc_open(SfNsClient*,uint32_t,const char*,const char*,unsigned,uint32_t,uint32_t);
int sf_nsc_openat(SfNsClient*,uint32_t,uint64_t,const char*,unsigned,uint32_t,uint32_t);
int sf_nsc_remove(SfNsClient*,uint32_t,uint64_t,const char*,uint32_t,uint32_t,int);
int sf_nsc_stat(SfNsClient*,uint32_t,uint64_t,SfFileStat*);
int sf_nsc_dup(SfNsClient*,uint32_t,uint64_t);
int sf_nsc_close(SfNsClient*,uint32_t,uint64_t);
int64_t sf_nsc_read(SfNsClient*,uint32_t,uint64_t,void*,size_t,uint64_t,int);
int64_t sf_nsc_write(SfNsClient*,uint32_t,uint64_t,const void*,size_t);
int sf_nsc_truncate(SfNsClient*,uint32_t,uint64_t,uint64_t);
int64_t sf_nsc_seek(SfNsClient*,uint32_t,uint64_t,uint64_t);
int64_t sf_nsc_poll(SfNsClient*,uint32_t,uint64_t);
int64_t sf_nsc_enumerate(SfNsClient*,uint32_t,uint64_t,char*,size_t);
int sf_nsc_handoff(SfNsClient*,uint32_t,uint64_t,uint32_t);
int sf_nsc_reap(SfNsClient*,uint32_t);
int sf_nsc_unmount(SfNsClient*,unsigned);
#endif
