#ifndef SF_PERSIST_TERMINAL_H
#define SF_PERSIST_TERMINAL_H
#include "sf.h"
#include "persist_snapshot.h"
#include "persist_verify.h"

/* Private provider state, never a wire structure or a Root restart permit.
 * A valid STOP consumes its monotonic nonce even when quiescence says BUSY. */
typedef struct {uint64_t last_nonce;int accepted;} SfPersistTerminal;
#define SF_PERSIST_STOP_NS 3000000000ull
int persist_terminal_dispatch(SfPersistTerminal*,int64_t,SfMessage*,uint64_t now_ns);
/* The actual receive loop used by sf-persist; accepted STOP returns zero only
 * after one bounded ACK attempt, irrespective of its result. Root must still
 * independently confirm the exact provider PID's death. The sender comes
 * from persist.c so its signed test fixtures retain their sf_send wrappers. */
int64_t persist_serve(const SfBootArgs*,int(*ordinary)(SfMessage*),
                     int64_t(*reply_send)(uint32_t,SfMessage*));
int64_t persist_serve_with_idle(const SfBootArgs*,int(*ordinary)(SfMessage*),
                              int64_t(*reply_send)(uint32_t,SfMessage*),int(*unbound)(void),SfPersistSnapshotGet,SfPersistVolumeVerify);
#endif
