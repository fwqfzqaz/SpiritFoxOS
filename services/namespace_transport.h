#ifndef SF_NS_ROOT_TRANSPORT_H
#define SF_NS_ROOT_TRANSPORT_H
#include "sf.h"
/* Root-owned syscall adapters. The receive adapter bounds one wait by the
 * passed milliseconds; pump serves one authenticated Fs callback. */
typedef struct {
 int64_t(*send)(void*,uint32_t,SfMessage*);
 int64_t(*receive)(void*,uint32_t,SfMessage*,unsigned);
 uint64_t(*ticks)(void*);
 int(*alive)(void*,uint32_t);
 int(*pump)(void*,unsigned);
 void(*close)(void*,uint64_t);
} SfNsTransportOps;
typedef struct {SfNsTransportOps ops;void*context;uint32_t service,reply,peer,inflight;} SfNsTransport;
/* Returns actual receiver-authenticated sender, never message.status. The
 * caller retains its nonce and performs operation-specific response checks. */
int64_t sf_nst_exchange(SfNsTransport*,SfMessage*);
#endif
