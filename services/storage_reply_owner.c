#include "storage_reply_owner.h"

static int pid_valid(uint32_t pid){return pid>1&&pid<=INT32_MAX;}
static int handle_valid(uint32_t handle){return handle>=1&&handle<=INT32_MAX;}

int storage_reply_seal_valid(const SfStorageReplySeal*s){
 if(!s||s->reserved)return 0;
 const SfPersistStageBinding*b=&s->binding;
 uint64_t instance=b->storage_instance;
 if(!b->token||!(uint32_t)(instance>>32)||!(uint32_t)instance||
    b->writable!=1||b->version!=1||b->capacity%512u||b->capacity<(uint64_t)SF_TXN_SECTORS*512u||
    b->snapshot.size!=sizeof(SfPersistSnapshot)||b->snapshot.version!=1||
    !b->snapshot.generation||b->snapshot.notes_bytes>SF_TXN_FILE_BYTES||
    !pid_valid(b->driver_pid)||!pid_valid(b->fs_pid)||!pid_valid(s->candidate_pid)||
    b->driver_pid==b->fs_pid||b->driver_pid==s->candidate_pid||b->fs_pid==s->candidate_pid||
    !handle_valid(s->driver_ep)||!handle_valid(s->driver_reply)||s->driver_ep==s->driver_reply)return 0;
 return 1;
}

int storage_reply_acquire(SfStorageReplyOwner*o,const SfStorageReplySeal*s){
 if(!o||!s)return SF_INVALID;
 SfStorageReplySeal copy=*s;
 if(o->phase==SF_STORAGE_REPLY_BORROWED)return SF_BUSY;
 if(o->phase==SF_STORAGE_REPLY_TAINTED||o->phase!=SF_STORAGE_REPLY_ROOT)return SF_DRAINING;
 if(!storage_reply_seal_valid(&copy))return SF_INVALID;
 if(o->serial==UINT64_MAX)return SF_NO_SPACE;
 o->serial++;o->seal=copy;o->phase=SF_STORAGE_REPLY_BORROWED;o->error=0;return 0;
}

static int matching_lease(const SfStorageReplyOwner*o,const SfStorageReplySeal*s,uint64_t serial){
 return serial&&serial==o->serial&&storage_reply_seal_valid(s)&&
        storage_reply_seal_valid(&o->seal)&&!memcmp(&o->seal,s,sizeof(*s));
}

int storage_reply_release(SfStorageReplyOwner*o,const SfStorageReplySeal*s,uint64_t serial){
 if(!o||!s)return SF_INVALID;
 if(o->phase==SF_STORAGE_REPLY_TAINTED)return SF_DRAINING;
 if(o->phase!=SF_STORAGE_REPLY_BORROWED)return o->phase==SF_STORAGE_REPLY_ROOT?SF_STALE_HANDLE:SF_DRAINING;
 if(!matching_lease(o,s,serial))return SF_STALE_HANDLE;
 o->phase=SF_STORAGE_REPLY_ROOT;return 0;
}

int storage_reply_taint(SfStorageReplyOwner*o,const SfStorageReplySeal*s,uint64_t serial,int error){
 if(!o||!s)return SF_INVALID;
 if(o->phase==SF_STORAGE_REPLY_TAINTED)return SF_DRAINING;
 if(o->phase!=SF_STORAGE_REPLY_BORROWED)return o->phase==SF_STORAGE_REPLY_ROOT?SF_STALE_HANDLE:SF_DRAINING;
 if(!matching_lease(o,s,serial))return SF_STALE_HANDLE;
 if(error<SF_VERSION_REJECTED||error>=0)return SF_INVALID;
 o->phase=SF_STORAGE_REPLY_TAINTED;o->error=error;return 0;
}

int storage_reply_access(const SfStorageReplyOwner*o){
 return o&&o->phase==SF_STORAGE_REPLY_ROOT?0:SF_DRAINING;
}
