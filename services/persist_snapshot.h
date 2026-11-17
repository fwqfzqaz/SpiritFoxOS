#ifndef SF_PERSIST_SNAPSHOT_H
#define SF_PERSIST_SNAPSHOT_H
#include "package.h"
#include "txn.h"

/* Private versioned wire payload; hashes canonical bytes, never pointers or
 * native struct padding. It describes a cache, not an execution permit. */
typedef struct {
 uint32_t size,version;
 uint64_t generation,notes_bytes;
 uint8_t volume[16],files_sha256[32],trust_sha256[32];
} SfPersistSnapshot;
_Static_assert(sizeof(SfPersistSnapshot)==104,"Persist snapshot wire v1");
typedef int(*SfPersistSnapshotGet)(SfPersistSnapshot*);
int persist_snapshot_dispatch(int64_t,const SfMessage*,uint64_t,SfPersistSnapshotGet,SfPersistSnapshot*);
void persist_snapshot_reply(SfMessage*,int,uint64_t,const SfPersistSnapshot*);
int persist_snapshot_get(SfPersistSnapshot*);
/* Private unbound candidate cache only. Reads authenticated transaction
 * sectors and compares the complete old snapshot; no recovery write or mount. */
int persist_candidate_verify_volume(const SfPersistSnapshot*,SfTxnRead,void*,SfPersistSnapshot*);

enum { SF_PERSIST_CAPTURE_NONE,SF_PERSIST_CAPTURE_REQUEST_UNKNOWN,
 SF_PERSIST_CAPTURE_CAPTURED,SF_PERSIST_CAPTURE_REFUSED,SF_PERSIST_CAPTURE_FAILED };
typedef struct {
 uint64_t token,request_id,deadline_ns;
 uint32_t pid;unsigned phase;
 int error,authenticated;
 int64_t send_result,receive_result,peer_status;
 SfPersistSnapshot snapshot;
} SfPersistCaptureStatus;
int sf_persist_fence_capture(uint64_t);
int sf_persist_fence_capture_status(uint64_t,SfPersistCaptureStatus*);
#endif
