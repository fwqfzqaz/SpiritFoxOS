#ifndef SF_PERSIST_CANDIDATE_H
#define SF_PERSIST_CANDIDATE_H
#include "package.h"

/* In-memory inspection result only. A prepared plan grants no capabilities
 * and does not authorize execution or Persist service readiness. */
typedef struct SfPersistCandidateImage {
 uint64_t bytes;
 uint8_t source_sha256[32],key_id[32];
 SfLoadPlan plan;
} SfPersistCandidateImage;

/* Authenticate the exact fixed Persist service image and prepare a static
 * process load plan in caller-owned scratch. No spawn, IPC, I/O, or global
 * trust/cache mutation occurs. staging is scratch, not an execution permit.
 * Invalid pointers, overflowing or aliased ranges leave buffers untouched;
 * after disjoint inputs are validated, all errors leave out zero. */
int sf_persist_candidate_prepare(const SfRuntimeTrust *trust,const void *data,
                                 uint64_t bytes,void *staging,size_t capacity,
                                 SfPersistCandidateImage *out);

/* Root-private, one isolated candidate cycle per sealed stop token. These
 * snapshots convey neither volume readiness nor activation authority. */
enum { SF_PERSIST_IDLE_NONE, SF_PERSIST_IDLE_ALLOCATING, SF_PERSIST_IDLE_SPAWNING,
 SF_PERSIST_IDLE_PROBING, SF_PERSIST_IDLE_DISCARDING, SF_PERSIST_IDLE_EXIT_WAIT,
 SF_PERSIST_IDLE_CLOSED, SF_PERSIST_IDLE_FAILED };
typedef struct {
 uint64_t token,probe_id,guard_id,discard_id,deadline_ns;
 uint32_t pid,ep,reply;
 unsigned phase,observations;
 int error,probe_accepted,guard_denied,discard_accepted,death_confirmed,retired;
 int64_t spawn_result,probe_send,probe_receive,discard_send,discard_receive;
 int64_t guard_send,guard_receive;
 int64_t task_status,revoke_ep_result,revoke_reply_result;
} SfPersistIdleStatus;
typedef int64_t(*SfPersistIdleSpawn)(const SfRuntimeTrust*,uint32_t ep,uint32_t reply,void*);
int sf_persist_candidate_cycle(uint64_t token,SfPersistIdleSpawn,void*);
int sf_persist_candidate_status(uint64_t token,SfPersistIdleStatus*);
#endif
