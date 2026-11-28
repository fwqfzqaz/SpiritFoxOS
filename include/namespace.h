#ifndef NAMESPACE_H
#define NAMESPACE_H
#include "sf.h"
enum { NODE_DIR=1,NODE_FILE,NODE_PIPE,NODE_CONSOLE,NODE_DISK,NODE_PROJECTION };
/* SFRWFS1-backed nodes encode their inode in SfNode.external with this bit.
 * UINT32_MAX is the legacy broker-backed NODE_DISK sentinel and must never be
 * classified as a formatted-volume node. */
#define SF_NS_RW_BIT 0x80000000u
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
/* Trusted in-process directory hooks installed by the SFRWFS1 runtime living
 * in the same process. Paths are namespace-absolute; return values are
 * negative SF_* codes. create() reports the formatted-volume inode through
 * *inode so the published node can route later I/O to the same object. */
typedef struct {
 int(*create)(void*,const char*,int,uint32_t,unsigned,uint32_t*);
 int(*remove)(void*,const char*,int,uint32_t);
 int(*rename)(void*,const char*,const char*,uint32_t);
 void*context;
} SfNsDirBackend;
/* One service-owned instance per process; initialise explicitly before use. */
SfNamespace*sf_ns_default(void);
void sf_ns_init(SfNamespace*,long(*)(const void*,size_t));
int sf_ns_mount_check(SfNamespace*,const char*);
int sf_ns_mount_publish(SfNamespace*,const SfMountEntry*,unsigned,unsigned);
SfNsRead sf_ns_external(SfNamespace*,SfNsRead,long(*)(unsigned,uint64_t,const void*,size_t),int(*)(unsigned,uint64_t));
void sf_ns_mount_hooks(SfNamespace*,int(*)(unsigned),void(*)(unsigned));
void sf_ns_dir_backend(SfNamespace*,const SfNsDirBackend*);
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
/* Absolute-path rename of a formatted-volume file or directory. Destination
 * parents must belong to the same writable mount; read-only nodes are
 * rejected rather than silently mirrored in RAM. */
int sf_ns_rename(SfNamespace*,uint32_t,const char*,const char*);
int sf_ns_rename_as(SfNamespace*,uint32_t,const char*,const char*,uint32_t,uint32_t);
int sf_ns_stat(SfNamespace*,uint32_t,uint32_t,SfFileStat*);
int sf_ns_dup(SfNamespace*,uint32_t,uint32_t);
int sf_ns_close(SfNamespace*,uint32_t,uint32_t);
long sf_ns_read(SfNamespace*,uint32_t,uint32_t,void*,size_t);
long sf_ns_pread(SfNamespace*,uint32_t,uint32_t,void*,size_t,uint64_t);
long sf_ns_write(SfNamespace*,uint32_t,uint32_t,const void*,size_t);
int sf_ns_truncate(SfNamespace*,uint32_t,uint32_t,uint64_t);
int64_t sf_ns_seek(SfNamespace*,uint32_t,uint32_t,uint64_t);
long sf_ns_poll(SfNamespace*,uint32_t,uint32_t);
long sf_ns_enumerate(SfNamespace*,uint32_t,uint32_t,char*,size_t);
long sf_ns_enumerate_at(SfNamespace*,uint32_t,uint32_t,uint32_t,char*,size_t);
void sf_ns_reap(SfNamespace*,uint32_t);
int sf_ns_unmount(SfNamespace*,unsigned);
#endif
