#ifndef SF_FILE_RPC_H
#define SF_FILE_RPC_H
#include "namespace.h"
/* Caller authenticates the sender and supplies that process's delegated root.
 * Every variable-length message access is bounded by the copied data field. */
long sf_file_dispatch(SfNamespace *,uint32_t owner,uint32_t *root,SfMessage *);
#endif
