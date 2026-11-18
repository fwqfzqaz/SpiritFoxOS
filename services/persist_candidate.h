#ifndef SF_PERSIST_CANDIDATE_H
#define SF_PERSIST_CANDIDATE_H
#include "package.h"
#include "persist_snapshot.h"
#include "persist_stage.h"

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
 SF_PERSIST_IDLE_CLOSED, SF_PERSIST_IDLE_FAILED, SF_PERSIST_IDLE_VERIFYING, SF_PERSIST_IDLE_PREPARED,
 SF_PERSIST_IDLE_STAGING, SF_PERSIST_IDLE_STAGED };
typedef struct {
 uint64_t token,probe_id,guard_id,discard_id,deadline_ns;
 uint32_t pid,ep,reply;
 unsigned phase,observations;
 int error,probe_accepted,guard_denied,discard_accepted,death_confirmed,retired;
 int64_t spawn_result,probe_send,probe_receive,discard_send,discard_receive;
 int64_t guard_send,guard_receive;
 int64_t task_status,revoke_ep_result,revoke_reply_result;
 uint64_t verify_id,verify_deadline_ns;
 unsigned verified,read_frames;
 int64_t verify_send,verify_receive;
 SfPersistSnapshot verified_snapshot;
} SfPersistIdleStatus;
typedef int64_t(*SfPersistIdleSpawn)(const SfRuntimeTrust*,uint32_t ep,uint32_t reply,void*);
int sf_persist_candidate_cycle(uint64_t token,SfPersistIdleSpawn,void*);
/* Root brokers only transaction-sector reads; success is still isolated and
 * followed by ordinary denial/discard, never service publication. */
int sf_persist_candidate_verify_cycle(uint64_t token,SfPersistIdleSpawn,void*,SfTxnRead,void*);
/* Retain an isolated, verified live owner until an explicit later discard.
 * These private calls do not publish a service or permit activation. */
int sf_persist_candidate_hold(uint64_t token,SfPersistIdleSpawn,void*,SfTxnRead,void*);
int sf_persist_candidate_discard(uint64_t token);
int sf_persist_candidate_status(uint64_t token,SfPersistIdleStatus*);
enum { SF_PERSIST_CAPS_NONE,SF_PERSIST_CAPS_ALLOCATING,SF_PERSIST_CAPS_SENDING,
       SF_PERSIST_CAPS_READY,SF_PERSIST_CAPS_FAILED,SF_PERSIST_CAPS_RETIRED };
typedef struct {
 uint64_t token,request_id[2],deadline[2];
 uint32_t parent[2];
 unsigned phase,branches_retired,allocation_unknown;
 int error;
 int64_t derive_result[2],send_result[2],receive_result[2],revoke_result[2];
 SfPersistStageBinding binding;
} SfPersistCapsStatus;
/* Provisionally held, attenuated driver capabilities only. Ordinary backend
 * globals, namespace admission and activation remain closed. */
int sf_persist_candidate_stage_caps(uint64_t token);
int sf_persist_candidate_caps_status(uint64_t token,SfPersistCapsStatus*);
/* Root-only preparatory reply-stream ownership; no candidate driver RPC.
 * Return is allowed only for a known completed use. Unknown completion must
 * quarantine instead; neither a status getter nor task death proves drain. */
int sf_persist_candidate_reply_borrow(uint64_t token,uint64_t*serial);
int sf_persist_candidate_reply_release(uint64_t token,uint64_t serial);
int sf_persist_candidate_reply_quarantine(uint64_t token,uint64_t serial,int error);
#endif
