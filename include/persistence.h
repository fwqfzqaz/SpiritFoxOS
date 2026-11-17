#ifndef SF_PERSISTENCE_H
#define SF_PERSISTENCE_H
#include "storage.h"
#include "namespace.h"
#include "package.h"
/* sf-init 侧客户端：持久化卷与更新状态机运行在 sf-persist.sfmod，
 * 本头文件只暴露有界 RPC 与命名空间投影。 */
int sf_persist_start(SfModuleSpawn spawn);
void sf_persist_attach(void);
int sf_persist_ready(void);
/* A mutating request with no authenticated completion keeps its dirty lease.
 * New package execution and volume RPC fail closed until a fresh boot. */
int sf_persist_uncertain(void);
/* New package authority requires a complete runtime-trust cache. A valid
 * explicit sync can repair this fault; it never clears an uncertain write. */
int sf_persist_security_error(void);
void sf_persist_driver_down(void);
int sf_persist_mount(SfNamespace*);
/* Existing mounted volume only. Neither operation mounts, recovers a trial,
 * synchronizes trust, nor writes storage. Reprojection needs an empty target. */
int sf_persist_notes_size(uint64_t*);
/* Point-in-time preflight only: no admission fence, drain, or stop. */
int sf_persist_quiescent(void);
/* Root-internal, non-destructive admission scope. No provider stop/start.
 * A nested refusal leaves the existing owner's output untouched. Otherwise
 * begin leaves *token nonzero only while this caller still owns a fence,
 * including an error whose changed context prevented safe cancellation.
 * cancel never repairs trust, an unknown write, or a changed incarnation. */
int sf_persist_fence_begin(uint64_t*token);
int sf_persist_fence_cancel(uint64_t token);
int sf_persist_fenced(void);
/* Root-private terminal ownership, not a wire ABI or a candidate/start permit.
 * stop sends once. Only authenticated acceptance, exact normal old-PID death,
 * unchanged ownership and successful endpoint retirement return zero. All
 * accepted/unknown outcomes keep admission closed and prohibit old cancel.
 * A fully authenticated refusal permits cancel only while the old context is
 * still healthy. No kill, retry, trust reset or DIRTY release is performed. */
enum { SF_PERSIST_TERMINAL_ACTIVE, SF_PERSIST_TERMINAL_REQUEST_UNKNOWN,
 SF_PERSIST_TERMINAL_ACCEPTED_WAIT, SF_PERSIST_TERMINAL_STOPPED_CLOSED,
 SF_PERSIST_TERMINAL_FAILED, SF_PERSIST_TERMINAL_REJECTED };
typedef struct {
 uint64_t token,request_id,deadline_ns,storage_instance,capacity;
 uint32_t pid,ep,reply,fs_pid,driver_pid,driver_ep,driver_reply;
 unsigned phase,observations;
 uint32_t ack_mode;
 int writable,ack_authenticated,death_confirmed,retired;
 int request_error,observation_error;
 int64_t send_result,receive_result,ack_status,task_status,revoke_ep_result,revoke_reply_result;
 SfRuntimeTrust trust;
} SfPersistStopStatus;
int sf_persist_fence_stop(uint64_t token);
/* Pure snapshot: never observes a process, retries IPC, or changes ownership.
 * The original tuple remains reportable after a legitimate refusal/cancel. */
int sf_persist_fence_stop_status(uint64_t token,SfPersistStopStatus*out);
/* Root-private inspection only. The trusted probe may prepare an image in
 * caller scratch, but never creates a process/capability or opens admission.
 * A returned description is not a later execution/activation permit. */
struct SfPersistCandidateImage;
typedef int (*SfPersistCandidateProbe)(const SfRuntimeTrust*,struct SfPersistCandidateImage*,void*);
int sf_persist_fence_inspect(uint64_t,SfPersistCandidateProbe,void*,struct SfPersistCandidateImage*);
/* The actual Root loader holds this scope through verification and spawn. */
int sf_persist_load_begin(uint64_t*token);
int sf_persist_load_end(uint64_t token);
int sf_persist_reproject(SfNamespace*);
/* 0 stages an update; 1 means the exact confirmed package is already present. */
int sf_persist_install(const char*);
int sf_persist_begin(void);
int sf_persist_finish(int success);
long sf_persist_package(void*,size_t);
/* Read-only authenticated preview; execution still requires begin + package. */
long sf_persist_preview(void*,size_t);
/* 0 revokes a stored package's signer; 1 means it was already revoked. */
int sf_persist_revoke(const char*);
void sf_persist_trust_status(void);
void sf_persist_status(void);
#endif
