#ifndef SF_STORAGE_REPLY_OWNER_H
#define SF_STORAGE_REPLY_OWNER_H
#include "persist_stage.h"

enum { SF_STORAGE_REPLY_ROOT=0, SF_STORAGE_REPLY_BORROWED=1,
       SF_STORAGE_REPLY_TAINTED=2 };

typedef struct {
 SfPersistStageBinding binding;
 uint32_t driver_ep,driver_reply,candidate_pid,reserved;
} SfStorageReplySeal;
_Static_assert(sizeof(SfStorageReplySeal)==160,"Storage reply lease seal v1");

typedef struct {
 uint64_t serial;
 unsigned phase;
 int error;
 SfStorageReplySeal seal;
} SfStorageReplyOwner;

/* Pure ownership primitive only. It neither authenticates the caller nor
 * rechecks live Root state; callers must validate that context themselves. */
int storage_reply_seal_valid(const SfStorageReplySeal*);
int storage_reply_acquire(SfStorageReplyOwner*,const SfStorageReplySeal*);
int storage_reply_release(SfStorageReplyOwner*,const SfStorageReplySeal*,uint64_t serial);
int storage_reply_taint(SfStorageReplyOwner*,const SfStorageReplySeal*,uint64_t serial,int error);
int storage_reply_access(const SfStorageReplyOwner*);
#endif
