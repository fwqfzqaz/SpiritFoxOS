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
#endif
