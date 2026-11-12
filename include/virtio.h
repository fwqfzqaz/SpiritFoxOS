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
/* ATA IDENTIFY word 117 holds the logical sector size in 16-bit words
 * (0/0xffff means the 512-byte default; word 118 is ignored like Linux does).
 * Only 512/1024/2048/4096 is accepted; blocks above 512 bytes require
 * read-modify-write for partial writes. */
static inline int sf_ata_logical_bytes(const uint16_t*id,uint32_t*bytes){
 uint32_t words=id[117],value=words*2;
 if(!words||words==0xffffu){*bytes=512;return 0;}
 if(value<512||value>4096||(value&(value-1)))return SF_BAD_FORMAT;
 *bytes=value;return 0;
}
/* Map a 512-byte-unit byte offset to the device logical block and the byte
 * offset inside it.  capacity is the device size in 512-byte units. */
static inline int sf_lba_split(uint64_t offset,uint32_t lba_bytes,uint64_t capacity,uint64_t*lba,uint32_t*within){
 uint32_t ratio;
 if(lba_bytes<512||lba_bytes>4096||(lba_bytes&(lba_bytes-1)))return SF_INVALID;
 ratio=lba_bytes/512;
 if((offset&511)||offset/512>=capacity||capacity%ratio)return SF_INVALID;
 *lba=offset/lba_bytes;*within=(uint32_t)(offset%lba_bytes);return 0;
}
/* Bridges the 512-byte OP_BLOCK_* protocol onto devices whose logical sector
 * is 512..4096 bytes.  The callback transfers exactly one logical block and
 * fills or consumes bridge->cache; the bridge caches one block and assembles
 * the two 256-byte halves, so a 512-byte write inside a larger block keeps
 * the other sectors by read-modify-write. */
typedef int (*SfBlockIo)(void*,int write,uint64_t lba);
typedef struct{
 void*context;SfBlockIo io;
 uint32_t lba_bytes,ratio,cache_valid,pending_valid,pending_off;
 uint64_t cache_lba,pending_lba;
 uint8_t cache[4096],pending[256];
}SfBlockBridge;
static inline void sf_bridge_init(SfBlockBridge*b,void*context,SfBlockIo io,uint32_t lba_bytes){
 b->context=context;b->io=io;b->lba_bytes=lba_bytes;b->ratio=lba_bytes/512;
 b->cache_valid=b->pending_valid=0;b->cache_lba=b->pending_lba=UINT64_MAX;
}
static inline int sf_bridge_read(SfBlockBridge*b,uint64_t offset,uint64_t capacity,uint32_t mode,uint8_t out[256]){
 uint64_t lba;uint32_t within;
 if(sf_lba_split(offset,b->lba_bytes,capacity,&lba,&within))return SF_INVALID;
 if(b->pending_valid)return SF_INVALID;
 if(!b->cache_valid||b->cache_lba!=lba){int r=b->io(b->context,0,lba);if(r){b->cache_valid=0;return r;}b->cache_lba=lba;b->cache_valid=1;}
 memcpy(out,b->cache+within+mode,256);return 0;
}
static inline int sf_bridge_write(SfBlockBridge*b,uint64_t offset,uint64_t capacity,uint32_t mode,const uint8_t in[256]){
 uint64_t lba;uint32_t within;
 if(sf_lba_split(offset,b->lba_bytes,capacity,&lba,&within)){b->pending_valid=0;return SF_INVALID;}
 if(!mode){memcpy(b->pending,in,256);b->pending_lba=lba;b->pending_off=within;b->pending_valid=1;return 0;}
 if(!b->pending_valid||b->pending_lba!=lba||b->pending_off!=within){b->pending_valid=0;return SF_INVALID;}
 b->pending_valid=0;
 if(b->ratio>1&&(!b->cache_valid||b->cache_lba!=lba)){int r=b->io(b->context,0,lba);if(r){b->cache_valid=0;return r;}b->cache_lba=lba;b->cache_valid=1;}
 if(b->ratio==1){memcpy(b->cache,b->pending,256);memcpy(b->cache+256,in,256);}else{memcpy(b->cache+within,b->pending,256);memcpy(b->cache+within+256,in,256);}
 int r=b->io(b->context,1,lba);
 if(r)b->cache_valid=0;else{b->cache_lba=lba;b->cache_valid=1;}
 return r;
}
static inline void sf_bridge_flush(SfBlockBridge*b){b->pending_valid=0;}
#endif
