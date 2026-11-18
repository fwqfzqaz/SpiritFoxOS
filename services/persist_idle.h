#ifndef SF_PERSIST_IDLE_H
#define SF_PERSIST_IDLE_H
#include "sf.h"

/* Private state for the provider's read-only idle-candidate protocol. */
typedef struct {
 uint64_t last_nonce;
 int entered,discarded;
} SfPersistIdle;

#define SF_PERSIST_IDLE_NS 3000000000ull

/* Pure message/state validation. The caller supplies the authenticated sender,
 * current monotonic time, and whether the service is currently unbound. */
int persist_idle_dispatch(SfPersistIdle*,int64_t sender,SfMessage*,
                          uint64_t now_ns,int unbound);

/* Normalize a reply in place. Kernel receive supplies the authenticated sender
 * flag; accepted is advisory transition metadata, never capability authority. */
void persist_idle_reply(SfMessage*,int status,uint64_t now_ns,int accepted);
#endif
