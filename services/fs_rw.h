#ifndef SF_FS_RW_H
#define SF_FS_RW_H
#include "sfrw.h"
/* Bridge SFRWFS1's 4096-byte block device onto 512-byte sector callbacks.
 * sf-fs supplies broker-backed callbacks; host tests supply memory ones. */
typedef struct {
 int (*read)(void*,uint64_t,uint8_t*);
 int (*write)(void*,uint64_t,const uint8_t*);
 int (*flush)(void*);
 void* context;
} SfRwIo;
#define SF_RW_SECTORS_PER_BLOCK (SF_RW_BLOCK_BYTES/512u)
_Static_assert(SF_RW_BLOCK_BYTES%512==0,"SFRWFS1 block must be sector aligned");
static inline int sf_rw_io_read(void*context,uint32_t block,uint8_t*out){
 SfRwIo*io=(SfRwIo*)context;
 if(!io||!io->read||!out)return SF_INVALID;
 for(unsigned i=0;i<SF_RW_SECTORS_PER_BLOCK;i++){
  int r=io->read(io->context,(uint64_t)block*SF_RW_SECTORS_PER_BLOCK+i,out+i*512u);
  if(r)return r;
 }
 return 0;
}
static inline int sf_rw_io_write(void*context,uint32_t block,const uint8_t*in){
 SfRwIo*io=(SfRwIo*)context;
 if(!io||!io->write||!in)return SF_READ_ONLY;
 for(unsigned i=0;i<SF_RW_SECTORS_PER_BLOCK;i++){
  int r=io->write(io->context,(uint64_t)block*SF_RW_SECTORS_PER_BLOCK+i,in+i*512u);
  if(r)return r;
 }
 return 0;
}
static inline int sf_rw_io_flush(void*context){
 SfRwIo*io=(SfRwIo*)context;
 return io&&io->flush?io->flush(io->context):0;
}
static inline SfrwDev sf_rw_io_dev(SfRwIo*io){
 SfrwDev dev;dev.read=sf_rw_io_read;dev.write=sf_rw_io_write;dev.flush=sf_rw_io_flush;dev.context=io;return dev;
}
#endif
