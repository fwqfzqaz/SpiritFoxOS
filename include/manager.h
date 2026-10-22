#ifndef MANAGER_H
#define MANAGER_H
#include "sf.h"
enum { MOD_DISCOVERED,MOD_VERIFIED,MOD_RESOLVED,MOD_STAGED,MOD_INITIALIZED,MOD_ACTIVE,MOD_QUIESCING,MOD_DRAINING,MOD_STOPPED,MOD_UNLOADED,MOD_FAILED,MOD_QUARANTINED };
enum { RES_LEASE,RES_OPEN,RES_REQUEST,RES_THREAD,RES_CALLBACK,RES_TIMER,RES_MAPPING,RES_DIRTY,RES_IRQ,RES_DMA,RES_COUNT };
typedef struct {char id[64];uint32_t generation,state,pinned,dependencies,resources[RES_COUNT];} SfModule;
typedef struct {SfModule modules[16];uint32_t next_generation;} SfManager;
int sfmod_add(SfManager*,const char*,uint32_t dependencies,int pinned);
int sfmod_start(SfManager*,unsigned);
int sfmod_acquire(SfManager*,unsigned,unsigned);
int sfmod_release(SfManager*,unsigned,unsigned);
int sfmod_prepare(SfManager*,unsigned);
int sfmod_cancel(SfManager*,unsigned);
int sfmod_stop(SfManager*,unsigned);
int sfmod_unload(SfManager*,unsigned);
int sfmod_cycle(const uint32_t*,unsigned,char*,size_t);
#endif
