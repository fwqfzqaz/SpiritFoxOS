/* E1: pure storage-reply owner state; no kernel, transport or I/O claim. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../services/storage_reply_owner.h"
#include "../../services/storage_reply_owner.c"

static unsigned checks,cases;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"STORAGE REPLY OWNER FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)

static SfStorageReplySeal seal_value(uint64_t tag){
 SfStorageReplySeal s;memset(&s,0,sizeof(s));
 s.binding.snapshot.size=sizeof(SfPersistSnapshot);s.binding.snapshot.version=1;
 s.binding.snapshot.generation=tag?tag:1;s.binding.snapshot.notes_bytes=SF_TXN_FILE_BYTES;
 s.binding.token=tag?tag:1;s.binding.storage_instance=(tag+1u)<<32|(tag+2u);
 s.binding.capacity=(uint64_t)SF_TXN_SECTORS*512u;
 s.binding.driver_pid=21;s.binding.fs_pid=22;s.binding.writable=1;s.binding.version=1;
 s.driver_ep=101;s.driver_reply=102;s.candidate_pid=23;
 return s;
}
static void unchanged(const SfStorageReplyOwner*o,const SfStorageReplyOwner*before){
 CHECK(!memcmp(o,before,sizeof(*o)));
}
static void mutate_seal(SfStorageReplySeal*s,unsigned field){
 switch(field){
  case 0:s->reserved=1;break;case 1:s->binding.token=0;break;
  case 2:s->binding.storage_instance&=0xffffffffull;break;
  case 3:s->binding.storage_instance&=0xffffffff00000000ull;break;
  case 4:s->binding.writable=0;break;case 5:s->binding.writable=2;break;
  case 6:s->binding.capacity++;break;
  case 7:s->binding.capacity=(uint64_t)SF_TXN_SECTORS*512u-512u;break;
  case 8:s->binding.snapshot.size++;break;case 9:s->binding.snapshot.version++;break;
  case 10:s->binding.snapshot.generation=0;break;
  case 11:s->binding.snapshot.notes_bytes=SF_TXN_FILE_BYTES+1u;break;
  case 12:s->binding.driver_pid=1;break;case 13:s->binding.fs_pid=0;break;
  case 14:s->candidate_pid=INT32_MAX+1u;break;
  case 15:s->binding.fs_pid=s->binding.driver_pid;break;
  case 16:s->candidate_pid=s->binding.driver_pid;break;
  case 17:s->candidate_pid=s->binding.fs_pid;break;
  case 18:s->driver_ep=0;break;case 19:s->driver_reply=INT32_MAX+1u;break;
  case 20:s->driver_reply=s->driver_ep;break;
  case 21:s->binding.snapshot.volume[0]^=1;break;
  case 22:s->binding.snapshot.files_sha256[0]^=1;break;
  case 23:s->binding.snapshot.trust_sha256[0]^=1;break;
  case 24:s->binding.version++;break;
  case 25:s->binding.driver_pid=INT32_MAX+1u;break;
  case 26:s->binding.fs_pid=INT32_MAX+1u;break;
  case 27:s->candidate_pid=1;break;
  case 28:s->driver_ep=INT32_MAX+1u;break;
  case 29:s->driver_reply=0;break;
  default:CHECK(0);break;
 }
}

static void seal_validation(void){
 SfStorageReplySeal s=seal_value(17);CHECK(storage_reply_seal_valid(&s));
 /* Zero snapshot digests are allowed: this primitive checks shape only. */
 for(unsigned i=0;i<32;i++){s.binding.snapshot.files_sha256[i]=0;s.binding.snapshot.trust_sha256[i]=0;}
 CHECK(storage_reply_seal_valid(&s));
 s=seal_value(19);s.binding.snapshot.volume[15]=1;CHECK(storage_reply_seal_valid(&s));
 s=seal_value(20);s.driver_ep=1;CHECK(storage_reply_seal_valid(&s));
 s=seal_value(21);s.driver_reply=1;CHECK(storage_reply_seal_valid(&s));
 s=seal_value(22);s.driver_ep=0;CHECK(!storage_reply_seal_valid(&s));
 s=seal_value(23);s.driver_reply=0;CHECK(!storage_reply_seal_valid(&s));
 s=seal_value(24);s.driver_ep=INT32_MAX+1u;CHECK(!storage_reply_seal_valid(&s));
 s=seal_value(25);s.driver_reply=INT32_MAX+1u;CHECK(!storage_reply_seal_valid(&s));
 for(unsigned i=0;i<30;i++){
  SfStorageReplySeal bad=seal_value(31+i);mutate_seal(&bad,i);
  if(i>=21&&i<=23)CHECK(storage_reply_seal_valid(&bad));
  else CHECK(!storage_reply_seal_valid(&bad));
 }
 CHECK(!storage_reply_seal_valid(0));cases++;
}

static void state_transitions_and_preservation(void){
 SfStorageReplyOwner o;memset(&o,0,sizeof(o));SfStorageReplySeal s=seal_value(41);
 CHECK(storage_reply_access(&o)==0);
 SfStorageReplyOwner before=o;
 CHECK(storage_reply_acquire(0,&s)==SF_INVALID);unchanged(&o,&before);
 CHECK(storage_reply_acquire(&o,0)==SF_INVALID);unchanged(&o,&before);
 SfStorageReplySeal bad=s;bad.reserved=1;
 CHECK(storage_reply_acquire(&o,&bad)==SF_INVALID);unchanged(&o,&before);
 CHECK(!storage_reply_acquire(&o,&s));CHECK(o.serial==1&&o.phase==SF_STORAGE_REPLY_BORROWED&&
       !o.error&&!memcmp(&o.seal,&s,sizeof(s))&&storage_reply_access(&o)==SF_DRAINING);
 before=o;SfStorageReplySeal other=seal_value(42);
 CHECK(storage_reply_acquire(&o,&other)==SF_BUSY);unchanged(&o,&before);
 CHECK(storage_reply_release(&o,&s,0)==SF_STALE_HANDLE);unchanged(&o,&before);
 CHECK(storage_reply_release(&o,&s,2)==SF_STALE_HANDLE);unchanged(&o,&before);
 CHECK(storage_reply_release(&o,&other,1)==SF_STALE_HANDLE);unchanged(&o,&before);
 CHECK(storage_reply_taint(&o,&other,1,SF_BUSY)==SF_STALE_HANDLE);unchanged(&o,&before);
 CHECK(!storage_reply_release(&o,&s,1));CHECK(o.phase==SF_STORAGE_REPLY_ROOT&&o.serial==1&&
       !o.error&&!memcmp(&o.seal,&s,sizeof(s))&&storage_reply_access(&o)==0);
 before=o;CHECK(storage_reply_release(&o,&s,1)==SF_STALE_HANDLE);unchanged(&o,&before);
 CHECK(!storage_reply_acquire(&o,&s));CHECK(o.serial==2&&o.phase==SF_STORAGE_REPLY_BORROWED);
 before=o;CHECK(storage_reply_release(&o,&s,1)==SF_STALE_HANDLE);unchanged(&o,&before);
 CHECK(!storage_reply_taint(&o,&s,2,SF_VERSION_REJECTED));
 CHECK(o.phase==SF_STORAGE_REPLY_TAINTED&&o.serial==2&&o.error==SF_VERSION_REJECTED&&
       !memcmp(&o.seal,&s,sizeof(s))&&storage_reply_access(&o)==SF_DRAINING);
 before=o;CHECK(storage_reply_acquire(&o,&s)==SF_DRAINING);unchanged(&o,&before);
 CHECK(storage_reply_release(&o,&s,2)==SF_DRAINING);unchanged(&o,&before);
 CHECK(storage_reply_taint(&o,&s,2,SF_BUSY)==SF_DRAINING);unchanged(&o,&before);
 cases++;
}

static void seal_drift_errors_and_unknown_states(void){
 SfStorageReplySeal s=seal_value(53);
 for(unsigned i=0;i<30;i++){
  SfStorageReplyOwner o;memset(&o,0,sizeof(o));CHECK(!storage_reply_acquire(&o,&s));
  SfStorageReplyOwner before=o;SfStorageReplySeal changed=s;mutate_seal(&changed,i);
  CHECK(storage_reply_release(&o,&changed,o.serial)==SF_STALE_HANDLE);unchanged(&o,&before);
  int err=storage_reply_taint(&o,&changed,o.serial,SF_BUSY);
  CHECK(err==SF_STALE_HANDLE);unchanged(&o,&before);
 }
 const int errors[]={SF_VERSION_REJECTED,-1,0,1,SF_VERSION_REJECTED-1};
 for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++){
  SfStorageReplyOwner o;memset(&o,0,sizeof(o));CHECK(!storage_reply_acquire(&o,&s));
  SfStorageReplyOwner before=o;int r=storage_reply_taint(&o,&s,o.serial,errors[i]);
  if(i<2){CHECK(!r&&o.phase==SF_STORAGE_REPLY_TAINTED&&o.error==errors[i]);}
  else{CHECK(r==SF_INVALID);unchanged(&o,&before);}
 }
 SfStorageReplyOwner o;memset(&o,0,sizeof(o));o.serial=UINT64_MAX;
 SfStorageReplyOwner before=o;CHECK(storage_reply_acquire(&o,&s)==SF_NO_SPACE);unchanged(&o,&before);
 memset(&o,0,sizeof(o));o.phase=99;before=o;
 CHECK(storage_reply_access(&o)==SF_DRAINING&&storage_reply_acquire(&o,&s)==SF_DRAINING&&
       storage_reply_release(&o,&s,1)==SF_DRAINING&&storage_reply_taint(&o,&s,1,SF_BUSY)==SF_DRAINING);
 unchanged(&o,&before);
 CHECK(storage_reply_access(0)==SF_DRAINING&&storage_reply_release(0,&s,1)==SF_INVALID&&
       storage_reply_taint(&o,0,1,SF_BUSY)==SF_INVALID);
 cases++;
}

static uint32_t next_random(uint32_t*x){*x=*x*1664525u+1013904223u;return*x;}
static void bounded_seeded_trajectories(void){
 uint32_t seed=0x51a7c0deu;
 for(unsigned i=0;i<1000;i++){
  SfStorageReplyOwner o;memset(&o,0,sizeof(o));SfStorageReplySeal s=seal_value((uint64_t)i+1000u);
  unsigned model_phase=SF_STORAGE_REPLY_ROOT;uint64_t model_serial=0;int model_error=0;
  for(unsigned step=0;step<6;step++){
   uint32_t choice=next_random(&seed);int r;
   if(step==0||(choice&3u)==0){
    r=storage_reply_acquire(&o,&s);
    if(model_phase==SF_STORAGE_REPLY_ROOT){model_serial++;model_phase=SF_STORAGE_REPLY_BORROWED;model_error=0;CHECK(!r);}
    else if(model_phase==SF_STORAGE_REPLY_BORROWED)CHECK(r==SF_BUSY);
    else CHECK(r==SF_DRAINING);
   }else if(choice&4u){
    r=storage_reply_release(&o,&s,model_serial);
    if(model_phase==SF_STORAGE_REPLY_BORROWED){model_phase=SF_STORAGE_REPLY_ROOT;CHECK(!r);}
    else if(model_phase==SF_STORAGE_REPLY_ROOT)CHECK(r==SF_STALE_HANDLE);
    else CHECK(r==SF_DRAINING);
   }else{
    int e=(choice&8u)?SF_BUSY:SF_TIMEOUT;
    r=storage_reply_taint(&o,&s,model_serial,e);
    if(model_phase==SF_STORAGE_REPLY_BORROWED){model_phase=SF_STORAGE_REPLY_TAINTED;model_error=e;CHECK(!r);}
    else if(model_phase==SF_STORAGE_REPLY_ROOT)CHECK(r==SF_STALE_HANDLE);
    else CHECK(r==SF_DRAINING);
   }
   CHECK(o.phase==model_phase&&o.serial==model_serial&&o.error==model_error);
   CHECK(storage_reply_access(&o)==(model_phase==SF_STORAGE_REPLY_ROOT?0:SF_DRAINING));
   /* This state predicate is the only gate a transport adapter may consume. */
   CHECK((storage_reply_access(&o)==0)==(model_phase==SF_STORAGE_REPLY_ROOT));
  }
  cases++;
 }
}

int main(void){
 seal_validation();state_transitions_and_preservation();seal_drift_errors_and_unknown_states();
 bounded_seeded_trajectories();
 printf("STORAGE REPLY OWNER PASS: %u checks/%u cases; pure primitive only\n",checks,cases);
 return 0;
}
