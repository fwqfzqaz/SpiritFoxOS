#ifndef SF_NAMESPACE_STATE_H
#define SF_NAMESPACE_STATE_H
#include "namespace.h"

/* Service implementation and deliberate white-box host fixtures only.
 * Clients use the opaque pointer and namespace functions. */
typedef struct {char path[96];uint8_t type,readonly,reserved;uint16_t mode,writers;uint32_t size,head,external,mount_tag,owner_uid,owner_gid;uint8_t data[1024];} SfNode;
typedef struct {uint32_t node,offset,mode,refs;} SfDescription;
typedef struct {uint32_t owner,generation,description;} SfFileHandle;

struct SfNamespace {SfNode nodes[SF_NS_NODE_MAX];SfDescription descriptions[SF_NS_DESCRIPTION_MAX];SfFileHandle handles[SF_NS_HANDLE_MAX];long(*console)(const void*,size_t);long(*external_read)(unsigned,uint64_t,void*,size_t);long(*external_write)(unsigned,uint64_t,const void*,size_t);int(*external_truncate)(unsigned,uint64_t);int(*mount_open)(unsigned);void(*mount_close)(unsigned);long(*projection_read)(unsigned,uint64_t,void*,size_t);};
#endif
