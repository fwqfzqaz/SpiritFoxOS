#ifndef SF_PERSIST_VERIFY_H
#define SF_PERSIST_VERIFY_H
#include "persist_snapshot.h"

#define SF_PERSIST_VERIFY_NS 30000000000ull
#define SF_PERSIST_VERIFY_READS (3u+2u*SF_TXN_BANK_SECTORS)
typedef int (*SfPersistVolumeVerify)(const SfPersistSnapshot*,SfTxnRead,void*,SfPersistSnapshot*);
typedef struct {uint64_t last_nonce;int attempted;} SfPersistVerify;
typedef struct {
 void*context;
 int64_t(*send)(void*,SfMessage*);
 int64_t(*receive)(void*,SfMessage*,uint64_t);
 int(*now)(void*,uint64_t*);
 void(*close)(void*,uint64_t);
} SfPersistVerifyTransport;
int persist_verify_dispatch(SfPersistVerify*,int64_t,const SfMessage*,uint64_t,int,
                            SfPersistVolumeVerify,const SfPersistVerifyTransport*,SfPersistSnapshot*);
void persist_verify_reply(SfMessage*,int,uint64_t,const SfPersistSnapshot*);
#endif
