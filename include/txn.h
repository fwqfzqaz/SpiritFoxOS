#ifndef SF_TXN_H
#define SF_TXN_H
#include "sf.h"
#define SF_TXN_SECTORS 4096u
#define SF_TXN_HEADER 2048u
#define SF_TXN_BANK 2051u
#define SF_TXN_BANK_SECTORS 256u
#define SF_TXN_FILES 5u
/* File data slots start after the bank header and all file records, so the
 * record area and slot 0 can never overlap regardless of SF_TXN_FILES. */
#define SF_TXN_DATA (512u+SF_TXN_FILES*128u)
#define SF_TXN_FILE_BYTES 16384u
typedef int (*SfTxnRead)(void*,uint64_t,uint8_t[512]);
typedef int (*SfTxnWrite)(void*,uint64_t,const uint8_t[512]);
typedef int (*SfTxnFlush)(void*);
typedef struct { char path[64]; uint32_t bytes; uint8_t data[SF_TXN_FILE_BYTES]; } SfTxnFile;
typedef struct {
 SfTxnRead read; SfTxnWrite write; SfTxnFlush flush; void*context;
 uint64_t generation; unsigned bank,ready; uint8_t volume[16];
 SfTxnFile files[SF_TXN_FILES],next[SF_TXN_FILES];
 uint8_t buffer[512*SF_TXN_BANK_SECTORS];
} SfTxn;
int sf_txn_mount(SfTxn*,SfTxnRead,SfTxnWrite,SfTxnFlush,void*);
long sf_txn_get(SfTxn*,const char*,void*,size_t);
int sf_txn_commit(SfTxn*,const char*const*,const void*const*,const uint32_t*,unsigned);
int sf_txn_resize(SfTxn*,const char*,uint64_t);
#endif
