#ifndef SF_VIRTIO_H
#define SF_VIRTIO_H
#include "sf.h"
typedef struct {uint64_t address;uint32_t length;uint16_t flags,next;} SfVirtqDescriptor;
typedef struct {uint32_t type,reserved;uint64_t sector;} SfVirtioRequest;
typedef struct {uint64_t sectors;uint32_t sector_bytes,readonly;} SfBlockInfo;
_Static_assert(sizeof(SfVirtqDescriptor)==16,"virtq descriptor layout");
_Static_assert(sizeof(SfVirtioRequest)==16,"virtio block request layout");
static inline int sf_block_range(uint64_t capacity,uint64_t offset,uint32_t half,uint32_t length){return !(offset&511)&&half<=256&&!(half&255)&&length==256&&offset/512<capacity;}
static inline int sf_virtq_layout(unsigned size,uint32_t*used){if(size<3||size>256||(size&(size-1)))return SF_BAD_FORMAT;*used=(16u*size+6u+2u*size+4095u)&~4095u;return *used+6u+8u*size<=16384?0:SF_BAD_FORMAT;}
#endif
