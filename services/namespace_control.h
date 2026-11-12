#ifndef SF_NAMESPACE_CONTROL_H
#define SF_NAMESPACE_CONTROL_H
#include "namespace.h"
#include "namespace_protocol.h"
/* These callbacks and staging records are private C state, never wire data. */
typedef struct {
 long(*console)(const void*,size_t);
 SfNsRead read;
 long(*write)(unsigned,uint64_t,const void*,size_t);
 int(*truncate)(unsigned,uint64_t);
 int(*mount_open)(unsigned);
 void(*mount_close)(unsigned);
 SfNsRead projection;
} SfNsBackends;
typedef struct {
 SfNamespace*ns;
 SfNsBackends backends;
 uint64_t token;
 uint32_t ready,count,next,tag,mask;
 SfMountEntry entries[SF_NS_NODE_MAX];
} SfNsControl;
void sf_nsr_init(SfNsControl*,SfNamespace*,const SfNsBackends*);
/* Actual kernel receive sender, never a user-provided actor. The wrapper must
 * close unexpected transferred capabilities and clear reply cap/deadline. */
int64_t sf_nsr_dispatch(SfNsControl*,uint32_t,SfMessage*);
#endif
