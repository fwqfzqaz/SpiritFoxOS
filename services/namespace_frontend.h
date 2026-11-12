#ifndef SF_NAMESPACE_FRONTEND_H
#define SF_NAMESPACE_FRONTEND_H
#include "namespace.h"
/* Manager-private lifecycle/callback entry points; not application RPC. */
void sf_ns_peer_lost(void);
int sf_ns_client_healthy(void);
int64_t sf_ns_client_callback(uint32_t,SfMessage*);
/* Implemented by storage's authenticated bounded endpoint/broker owner. */
int sf_storage_namespace_start(void);
int64_t sf_storage_namespace_exchange(void*,SfMessage*);
int sf_storage_namespace_disk_live(void);
void sf_init_namespace_lost(void);
#endif
