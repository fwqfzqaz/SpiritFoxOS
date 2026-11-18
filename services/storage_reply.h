#ifndef SF_STORAGE_REPLY_H
#define SF_STORAGE_REPLY_H
#include "storage_reply_owner.h"

/* Root-private, single-threaded storage transport ownership. These trusted
 * adapters are not RPCs or application SDK. The candidate controller must
 * authenticate its live STAGED binding before claim/known-completion return.
 * No operation issues driver RPC, drains messages, kills or resets a device. */
int sf_storage_reply_claim_trusted(const SfStorageReplySeal*,uint64_t*);
int sf_storage_reply_return_trusted(const SfStorageReplySeal*,uint64_t);
int sf_storage_reply_taint_trusted(const SfStorageReplySeal*,uint64_t,int);
/* History only; access is the actual pre-transport gate, not a health probe. */
int sf_storage_reply_status(SfStorageReplyOwner*);
int sf_storage_reply_access(void);
#endif
