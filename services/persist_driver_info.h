#ifndef SF_PERSIST_DRIVER_INFO_H
#define SF_PERSIST_DRIVER_INFO_H
#include "persist_stage.h"
#include "virtio.h"

typedef struct {
 SfPersistStageBinding binding;
 SfBlockInfo info;
} SfPersistDriverInfo;
_Static_assert(sizeof(SfPersistDriverInfo)==160,"Persist driver inquiry reply v1");

typedef struct {
 uint64_t last_nonce;
 int attempted, fatal;
} SfPersistDriverInquiry;

typedef struct {
 void *context;
 int64_t (*send)(void*,uint32_t,SfMessage*);
 int64_t (*receive)(void*,uint32_t,SfMessage*,uint64_t);
 int64_t (*close)(void*,uint64_t);
 int (*now)(void*,uint64_t*);
} SfPersistDriverTransport;

/* With valid transport, consumes an unexpected installed capability on either
 * ingress path. An uncertain close marks fatal so the provider must exit.
 * The caller synchronizes last_nonce with the candidate control stream. */
int persist_driver_info_dispatch(SfPersistDriverInquiry*,const SfPersistStage*,
                                int64_t,const SfMessage*,uint64_t,int,
                                const SfPersistDriverTransport*,SfPersistDriverInfo*);
void persist_driver_info_reply(SfMessage*,int,uint64_t,const SfPersistDriverInfo*);
#endif
