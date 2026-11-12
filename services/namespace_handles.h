#ifndef SF_NAMESPACE_HANDLES_H
#define SF_NAMESPACE_HANDLES_H
#include "namespace.h"

/* Manager-private indirection for the namespace RPC migration. Never put these
 * records on the wire. Construct once for the manager's lifetime; peer loss
 * invalidates bindings while retaining generations. This module does not
 * authenticate replies or close remote handles: the future transport owns
 * those duties and must invalidate bindings on an uncertain outcome.
 *
 * Reservations are snapshots, not locks or unique request IDs. Serialize
 * requests, correlate replies separately, and commit only a confirmed OPEN or
 * DUP reply. A reservation cannot survive slot reuse or a peer change. */
#define SF_NSC_GENERATION_MAX 0x7fffffu
typedef struct {uint32_t owner,generation,remote;} SfNsClientHandle;
typedef struct {uint32_t slot,generation,owner;uint64_t epoch;} SfNsHandleReservation;
typedef struct {
 SfNsClientHandle handles[SF_NS_HANDLE_MAX];
 uint64_t epoch;
 uint32_t exhausted;
} SfNsHandleMap;
void sf_nsh_init(SfNsHandleMap*);
int sf_nsh_prepare(const SfNsHandleMap*,uint32_t,SfNsHandleReservation*);
int sf_nsh_commit(SfNsHandleMap*,const SfNsHandleReservation*,uint64_t);
int sf_nsh_lookup(const SfNsHandleMap*,uint32_t,uint64_t,uint32_t*);
int sf_nsh_forget(SfNsHandleMap*,uint32_t,uint64_t);
int sf_nsh_handoff(SfNsHandleMap*,uint32_t,uint64_t,uint32_t);
void sf_nsh_reap(SfNsHandleMap*,uint32_t);
void sf_nsh_invalidate(SfNsHandleMap*);
#endif
