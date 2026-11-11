#ifndef NAMESPACE_H
#define NAMESPACE_H
#include "sf.h"
enum { NODE_DIR=1,NODE_FILE,NODE_PIPE,NODE_CONSOLE,NODE_DISK,NODE_PROJECTION };
#define SF_NS_NODE_MAX 128
/* 句柄槽号编码在低 8 位，容量上限不能超过 255。 */
#define SF_NS_DESCRIPTION_MAX 64
#define SF_NS_HANDLE_MAX 128
enum { OPEN_READ=1,OPEN_WRITE=2,OPEN_NONBLOCK=4,OPEN_DIRECTORY=8 };
/* mode holds the classic nine permission bits (owner/group/other rwx).
 * uid 0 bypasses permission checks; ownership is assigned by the trusted
 * init when it materialises account homes and shared directories. */
/* Trusted in-process mount declarations, not a wire ABI. Parents precede
 * children; the first entry is the mount directory. Publication is atomic. */
typedef struct {char path[96];uint32_t type,readonly,external,size;} SfMountEntry;
typedef long(*SfNsRead)(unsigned,uint64_t,void*,size_t);
typedef struct SfNamespace SfNamespace;
/* One service-owned instance per process; initialise explicitly before use. */
SfNamespace*sf_ns_default(void);
void sf_ns_init(SfNamespace*,long(*)(const void*,size_t));
int sf_ns_mount_check(SfNamespace*,const char*);
int sf_ns_mount_publish(SfNamespace*,const SfMountEntry*,unsigned,unsigned);
SfNsRead sf_ns_external(SfNamespace*,SfNsRead,long(*)(unsigned,uint64_t,const void*,size_t),int(*)(unsigned,uint64_t));
void sf_ns_mount_hooks(SfNamespace*,int(*)(unsigned),void(*)(unsigned));
void sf_ns_projection_backend(SfNamespace*,SfNsRead);
/* Manager-only handoff: move one handle, retaining its description and lease.
 * This is deliberately absent from the untrusted file RPC dispatcher. */
int sf_ns_handoff(SfNamespace*,uint32_t,uint32_t,uint32_t);
int sf_ns_create(SfNamespace*,const char*,unsigned,int);
int sf_ns_create_owned(SfNamespace*,const char*,unsigned,int,uint32_t,uint32_t,unsigned);
int sf_ns_assign(SfNamespace*,const char*,uint32_t,uint32_t,unsigned);
int sf_ns_project(SfNamespace*,const char*,unsigned);
int sf_ns_protect(SfNamespace*,const char*);
int sf_ns_open(SfNamespace*,uint32_t,const char*,const char*,unsigned);
int sf_ns_open_as(SfNamespace*,uint32_t,const char*,const char*,unsigned,uint32_t,uint32_t);
int sf_ns_openat(SfNamespace*,uint32_t,uint32_t,const char*,unsigned);
int sf_ns_openat_as(SfNamespace*,uint32_t,uint32_t,const char*,unsigned,uint32_t,uint32_t);
int sf_ns_unlink(SfNamespace*,uint32_t,uint32_t,const char*);
int sf_ns_unlink_as(SfNamespace*,uint32_t,uint32_t,const char*,uint32_t,uint32_t);
int sf_ns_rmdir(SfNamespace*,uint32_t,uint32_t,const char*);
int sf_ns_rmdir_as(SfNamespace*,uint32_t,uint32_t,const char*,uint32_t,uint32_t);
int sf_ns_stat(SfNamespace*,uint32_t,uint32_t,SfFileStat*);
int sf_ns_dup(SfNamespace*,uint32_t,uint32_t);
int sf_ns_close(SfNamespace*,uint32_t,uint32_t);
long sf_ns_read(SfNamespace*,uint32_t,uint32_t,void*,size_t);
long sf_ns_pread(SfNamespace*,uint32_t,uint32_t,void*,size_t,uint64_t);
long sf_ns_write(SfNamespace*,uint32_t,uint32_t,const void*,size_t);
int sf_ns_truncate(SfNamespace*,uint32_t,uint32_t,uint64_t);
long sf_ns_seek(SfNamespace*,uint32_t,uint32_t,uint64_t);
long sf_ns_poll(SfNamespace*,uint32_t,uint32_t);
long sf_ns_enumerate(SfNamespace*,uint32_t,uint32_t,char*,size_t);
void sf_ns_reap(SfNamespace*,uint32_t);
int sf_ns_unmount(SfNamespace*,unsigned);
#endif
