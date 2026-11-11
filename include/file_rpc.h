#ifndef SF_FILE_RPC_H
#define SF_FILE_RPC_H
#include "namespace.h"
/* Caller authenticates the sender and supplies that process's delegated root.
 * Every variable-length message access is bounded by the copied data field. */
int64_t sf_file_dispatch(SfNamespace *,uint32_t owner,uint32_t *root,SfMessage *);
int64_t sf_file_dispatch_as(SfNamespace *,uint32_t owner,uint32_t *root,SfMessage *,uint32_t uid,uint32_t gid);
#endif
