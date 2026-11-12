#ifndef SF_NAMESPACE_RUNTIME_H
#define SF_NAMESPACE_RUNTIME_H
#include "namespace_control.h"
#include "readfs.h"
typedef int64_t(*SfNsBrokerExchange)(void*,SfMessage*);
void sf_nsv_init(SfReadFs*,SfNsBrokerExchange,void*);
int64_t sf_nsv_dispatch(uint32_t,SfMessage*);
int sf_nsv_publish_disk(uint64_t);
/* The process wrapper must exit before replying if a callback became
 * uncertain, especially after a mount-close whose C callback is void. */
int sf_nsv_faulted(void);
int sf_nsv_ready(void);
#endif
