#ifndef SF_PERSIST_STAGE_H
#define SF_PERSIST_STAGE_H
#include "persist_snapshot.h"

#define SF_PERSIST_STAGE_NS 3000000000ull
enum { SF_PERSIST_STAGE_SEND=1, SF_PERSIST_STAGE_REPLY=2 };
enum { SF_PERSIST_STAGE_NONE, SF_PERSIST_STAGE_SEND_HELD,
       SF_PERSIST_STAGE_READY, SF_PERSIST_STAGE_FAILED };
/* Private versioned wire payload. No ordinary driver binding is installed. */
typedef struct {
 SfPersistSnapshot snapshot;
 uint64_t token,storage_instance,capacity;
 uint32_t driver_pid,fs_pid,writable,version;
} SfPersistStageBinding;
_Static_assert(sizeof(SfPersistStageBinding)==144,"Persist provisional caps wire v1");
typedef struct {
 uint64_t last_nonce;
 uint32_t send,reply;
 unsigned phase,fatal;
 SfPersistStageBinding binding;
} SfPersistStage;
typedef struct {
 void*context;
 int64_t(*derive)(void*,uint64_t,unsigned);
 int64_t(*close)(void*,uint64_t);
} SfPersistStageCaps;
/* With valid configuration, consumes ownership of a cap_count=1 receipt on
 * every path. A failed CLOSE sets fatal: caller must exit, never keep serving.
 * A STALE close is known already absent. No storage callbacks are available. */
int persist_stage_dispatch(SfPersistStage*,int64_t,const SfMessage*,uint64_t,
                           int available,uint32_t self,const SfPersistSnapshot*,
                           const SfPersistStageCaps*);
void persist_stage_reply(SfMessage*,int,uint64_t,unsigned,const SfPersistStageBinding*);
#endif
