/* E1: compile the actual Root client/storage owners and real resource and
 * namespace tables. Only kernel identity, time and transport are simulated.
 * This does not terminate a guest or authorize a future candidate/start. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LIBSF_H
#include "sf.h"
static int64_t sf_sys(long,uint64_t,uint64_t,uint64_t);
static int64_t sf_send(uint32_t,SfMessage*);
static int64_t sf_recv(uint32_t,SfMessage*,uint64_t);
static long sf_cap_close(uint64_t);
static long sf_cap_derive(uint64_t,uint64_t);
static long sf_send_cap(uint32_t,SfMessage*,uint64_t);
static uint64_t sf_ticks(void);
static void sf_yield(void);
#ifdef SF_TEST_STAGE_OWNER
static int caps_owner_sys_hook(long,uint64_t,uint64_t,uint64_t,int64_t*);
static int caps_owner_send_cap_hook(uint32_t,SfMessage*,uint64_t,int64_t*);
static int caps_owner_recv_hook(uint32_t,SfMessage*,uint64_t,int64_t*);
#endif
#ifdef SF_TEST_IDLE_OWNER
static int idle_owner_sys_hook(long,uint64_t,uint64_t,uint64_t,int64_t*);
static int idle_owner_send_hook(uint32_t,SfMessage*,int64_t*);
static int idle_owner_recv_hook(uint32_t,SfMessage*,uint64_t,int64_t*);
#endif
static uint64_t sf_deadline(unsigned ms){return sf_ticks()*10000000ull+(uint64_t)ms*1000000ull;}
static void sf_message(SfMessage*m,unsigned op,uint64_t id){memset(m,0,sizeof(*m));m->h=(SfIpcHeader){40,1,0,op,0,id,sizeof(*m)-40,0,sf_deadline(3000)};}
static void sf_log(const char*s){(void)s;}
static void sf_number(uint64_t n){(void)n;}
static void sf_assert(int,const char*);
#include "../../services/storage.c"
#include "../../services/persist_client.c"
#include "../../services/namespace_state.h"

static unsigned checks,cases,variant,mutation,sends,receives,advisories,closes,revokes,yields,polls;
static unsigned running_polls,freeze_clock,reenter,revoke_fail_at;
static int64_t send_result,recv_result,final_status,pre_status,revoke_error,clock_error;
static int semantic,namespace_alive;
static uint64_t clock_ticks;
static SfMessage reply;
static SfManager ledger,ledger_before;
static SfNamespace space,space_before;
static SfRuntimeTrust trust_before;
static SfTrustKey policy_keys[2];
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"PERSIST TERMINAL OWNER FAIL line %d variant%u mutation%u: %s\n",__LINE__,variant,mutation,#c);exit(1);}}while(0)

static void mutate(unsigned);
static int no_spawn(const char*p,uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e){(void)p;(void)a;(void)b;(void)c;(void)d;(void)e;CHECK(0);return SF_DENIED;}
static int64_t sf_sys(long op,uint64_t a,uint64_t b,uint64_t c){
#ifdef SF_TEST_STAGE_OWNER
 int64_t caps_result;if(caps_owner_sys_hook(op,a,b,c,&caps_result))return caps_result;
#endif
#ifdef SF_TEST_IDLE_OWNER
 int64_t hook_result;if(idle_owner_sys_hook(op,a,b,c,&hook_result))return hook_result;
#endif
 (void)b;(void)c;
 if(op==SYS_TICKS)return clock_error?clock_error:(int64_t)clock_ticks;
 if(op==SYS_YIELD){yields++;if(!freeze_clock)clock_ticks++;return 0;}
 if(op==SYS_TASK_STATUS){
  if(a!=12){CHECK(a==fs_pid||a==driver_pid);return SF_TASK_RUNNING;}
  CHECK(a==12||a==persist_pid);
  if(!sends)return pre_status;
  polls++;
  if(running_polls){running_polls--;return SF_TASK_RUNNING;}
  return final_status;
 }
 if(op==SYS_CAP_REVOKE){
  CHECK(sends==1&&terminal_stop.ack_authenticated&&terminal_stop.death_confirmed);
  CHECK(terminal_stop.task_status==0&&a==(revokes?0x102u:0x101u));
  CHECK(terminal_stop.phase==SF_PERSIST_TERMINAL_ACCEPTED_WAIT);
  revokes++;return !revoke_fail_at||revokes==revoke_fail_at?revoke_error:0;
 }
 /* In particular there is no supported kill or allocation fallback. */
 CHECK(0);return SF_DENIED;
}
static uint64_t sf_ticks(void){return clock_ticks;}
static void sf_yield(void){sf_sys(SYS_YIELD,0,0,0);}
static void sf_assert(int ok,const char*why){(void)why;CHECK(ok);}
static long sf_cap_close(uint64_t cap){CHECK(cap==0x777);closes++;return 0;}
static long sf_cap_derive(uint64_t cap,uint64_t rights){(void)cap;(void)rights;CHECK(0);return SF_DENIED;}
static long sf_send_cap(uint32_t ep,SfMessage*m,uint64_t cap){
#ifdef SF_TEST_STAGE_OWNER
 m->h.cap_count=1;m->handle=cap;
 int64_t caps_result;if(caps_owner_send_cap_hook(ep,m,cap,&caps_result))return caps_result;
#endif
 (void)ep;(void)m;(void)cap;CHECK(0);return SF_DENIED;
}
int sf_ns_client_healthy(void){return namespace_alive;}
void sf_ns_peer_lost(void){CHECK(0);}
int64_t sf_ns_client_callback(uint32_t from,SfMessage*m){(void)from;(void)m;CHECK(0);return SF_DENIED;}
static void locked(void){
 unsigned before=sends;
 CHECK(sf_persist_fenced()&&sf_persist_fence_cancel(admission_fence.token)==SF_DRAINING);
 CHECK(sf_persist_fence_stop(admission_fence.token)==SF_DRAINING);
 uint64_t token=0xfeed;CHECK(sf_persist_load_begin(&token)==SF_DRAINING&&token==0xfeed);
 CHECK(sf_storage_transaction(1)==SF_DRAINING&&sf_storage_unmount()==SF_DRAINING);
 CHECK(sf_persist_quiescent()==SF_DRAINING&&sends==before);
}
static int64_t sf_send(uint32_t ep,SfMessage*m){
#ifdef SF_TEST_IDLE_OWNER
 int64_t hook_result;if(idle_owner_send_hook(ep,m,&hook_result))return hook_result;
#endif
 CHECK(ep==0x101);
 if(m->h.opcode==OP_PERSIST_QUIESCENT){advisories++;CHECK(!sends);reply=*m;reply.h.flags=12;return 0;}
 CHECK(m->h.opcode==OP_PERSIST_STOP&&++sends==1);
 CHECK(m->h.size==40&&m->h.protocol_major==1&&!m->h.protocol_minor&&!m->h.flags&&!m->h.cap_count);
 CHECK(m->h.request_id==terminal_stop.request_id&&m->h.deadline_ns==terminal_stop.deadline_ns);
 CHECK(m->h.payload_bytes==sizeof(*m)-40&&m->h.deadline_ns==clock_ticks*10000000ull+3000000000ull);
 CHECK(!m->status&&!m->handle&&!m->offset&&!m->length&&!m->mode);
 for(unsigned i=0;i<sizeof(m->data);i++)CHECK(!m->data[i]);
 if(reenter)locked();
 reply=*m;reply.h.flags=12;reply.status=semantic;reply.mode=semantic?0u:1u;
 return send_result;
}
static int64_t sf_recv(uint32_t ep,SfMessage*m,uint64_t deadline){
#ifdef SF_TEST_STAGE_OWNER
 int64_t caps_result;if(caps_owner_recv_hook(ep,m,deadline,&caps_result))return caps_result;
#endif
#ifdef SF_TEST_IDLE_OWNER
 int64_t hook_result;if(idle_owner_recv_hook(ep,m,deadline,&hook_result))return hook_result;
#endif
 CHECK(ep==0x102);
 if(m->h.opcode==OP_PERSIST_QUIESCENT){CHECK(deadline==sf_deadline(10000));*m=reply;return 12;}
 receives++;CHECK(receives==1&&deadline==terminal_stop.deadline_ns);
 if(reenter)locked();
 if(mutation)mutate(mutation);
 if(recv_result)return recv_result;
 *m=reply;
 switch(variant){
 case 1:return 13;case 2:m->h.request_id++;break;case 3:m->h.size--;break;
 case 4:m->h.protocol_major++;break;case 5:m->h.protocol_minor++;break;
 case 6:m->h.opcode--;break;case 7:m->h.flags++;break;case 8:m->h.payload_bytes--;break;
 case 9:m->h.deadline_ns++;break;case 10:m->handle=1;break;case 11:m->offset=1;break;
 case 12:m->length=1;break;case 13:m->mode=2;break;case 14:m->status=1;break;
 case 15:m->status=1ll<<32;break;case 16:m->status=-(1ll<<32);break;
 case 17:m->status=SF_VERSION_REJECTED-1;break;
 case 18:m->h.cap_count=1;m->handle=0x777;break;case 19:m->h.cap_count=2;break;
 case 20:m->h.cap_count=1;m->handle=0x777;return 13;
 case 21:m->h.cap_count=1;m->handle=0x777;m->h.request_id++;break;
 case 22:m->data[0]=1;break;case 23:m->data[sizeof(m->data)-1]=1;break;
 case 24:return (1ll<<32)+12;case 25:m->h.request_id^=1ull<<32;break;
 case 26:m->h.deadline_ns=0;break;
 /* A status-only mutation of accepted ACK must not become a cancellable BUSY. */
 case 27:m->status=SF_BUSY;break;
 /* Neither an accepted status without the decision nor a refused status
  * accompanied by an accepted decision is a valid completion. */
 case 28:m->mode=0;break;case 29:m->status=SF_BUSY;m->mode=1;break;
 case 30:clock_error=SF_DENIED;break;
 case 31:clock_ticks=UINT64_MAX/10000000ull;break;
 }
 return 12;
}
static void reset(void){
 /* This is an isolated boot fixture reset, not a production reset API. */
 memset(&admission_fence,0,sizeof(admission_fence));memset(&terminal_stop,0,sizeof(terminal_stop));
 fence_sequence=load_sequence=load_owner=0;requests=0x300000000ull;
 memset(&ledger,0,sizeof(ledger));memset(&space,0,sizeof(space));manager=&ledger;ns=&space;spawn=no_spawn;
 driver_slot=sfmod_add(manager,"org.spiritfox.virtio.blk",0,0);fs_slot=sfmod_add(manager,"org.spiritfox.fs.disk",0,0);
 CHECK(driver_slot>=0&&fs_slot>=0&&!sfmod_start(manager,(unsigned)driver_slot)&&!sfmod_start(manager,(unsigned)fs_slot));
 CHECK(!sfmod_acquire(manager,(unsigned)driver_slot,RES_LEASE)&&!sfmod_acquire(manager,(unsigned)driver_slot,RES_DMA));
 driver_pid=22;driver_ep=0x201;driver_reply=0x202;fs_pid=21;
 fs_service_ep=0x301;fs_service_reply=0x302;fs_broker_ep=0x303;fs_broker_reply=0x304;
 persist_pid=12;persist_ep=0x101;persist_reply=0x102;
 mounted=writable=write_grant=mount_admission=mount_driver_lease=namespace_alive=1;
 capacity=SF_TXN_SECTORS*512ull;namespace_descriptions=0;memset(&fs_transport,0,sizeof(fs_transport));
 uncertain_commit=trust_error=0;disk_read=0;
 sends=receives=advisories=closes=revokes=yields=polls=0;variant=mutation=reenter=freeze_clock=running_polls=revoke_fail_at=0;
 send_result=recv_result=revoke_error=clock_error=semantic=0;final_status=0;pre_status=SF_TASK_RUNNING;clock_ticks=10;
 memset(&sf_trust,0,sizeof(sf_trust));sf_trust.policy.keys=policy_keys;sf_trust.policy.count=1;
 sf_trust.revoked=1;sf_trust.revoked_ids[0][0]=0x45;
 sf_ns_init(ns,0);sf_storage_namespace_bind(ns);
 const SfMountEntry disk[]={{"/disk",NODE_DIR,1,0,0},{"/disk/file",NODE_DISK,1,0,4}};
 CHECK(!sf_ns_mount_publish(ns,disk,2,1)&&!project_notes(ns,4));
 CHECK(!sf_ns_create(ns,"/ram",NODE_FILE,0));
}
static uint64_t begin(void){uint64_t token=0;unsigned before=advisories;CHECK(!sf_persist_fence_begin(&token)&&token&&advisories==before+1&&!sends);return token;}
static void snapshot_state(void){ledger_before=ledger;space_before=space;trust_before=sf_trust;}
static void unchanged(void){
 CHECK(!memcmp(&ledger,&ledger_before,sizeof(ledger))&&!memcmp(&space,&space_before,sizeof(space)));
 CHECK(!memcmp(&sf_trust,&trust_before,sizeof(sf_trust)));
 CHECK(mounted&&mount_admission&&mount_driver_lease&&namespace_alive);
}
static SfPersistStopStatus report(uint64_t token){
 SfPersistStopStatus result;memset(&result,0xa5,sizeof(result));
 CHECK(!sf_persist_fence_stop_status(token,&result)&&result.token==token);
 CHECK(result.pid==12&&result.ep==0x101&&result.reply==0x102&&result.fs_pid==21&&result.driver_pid==22);
 CHECK(result.driver_ep==0x201&&result.driver_reply==0x202&&result.capacity==SF_TXN_SECTORS*512ull&&result.writable==1);
 CHECK(result.storage_instance&&result.trust.revoked==1&&result.trust.revoked_ids[0][0]==0x45);
 return result;
}
static void no_retry(uint64_t token){
 unsigned old_sends=sends,old_receives=receives,old_polls=polls,old_revokes=revokes;
 CHECK(sf_persist_fence_stop(token)==SF_DRAINING&&sf_persist_fenced());
 CHECK(sf_persist_fence_cancel(token)==SF_DRAINING);
 SfPersistStopStatus a=report(token),b=report(token);CHECK(!memcmp(&a,&b,sizeof(a)));
 CHECK(sends==old_sends&&receives==old_receives&&polls==old_polls&&revokes==old_revokes);
}
static void mutate(unsigned kind){
 switch(kind){
 case 1:persist_pid++;break;case 2:persist_ep++;break;case 3:persist_reply++;break;
 case 4:fs_pid++;break;case 5:driver_pid++;break;case 6:driver_ep++;break;case 7:driver_reply++;break;
 case 8:capacity+=512;break;case 9:writable=0;break;case 10:ledger.modules[fs_slot].generation++;break;
 case 11:ledger.modules[driver_slot].generation++;break;case 12:ledger.modules[fs_slot].state=MOD_DRAINING;break;
 case 13:fs_transport.inflight=1;break;case 14:namespace_descriptions=1;break;case 15:namespace_alive=0;break;
 case 16:mounted=0;break;case 17:mount_admission=0;break;case 18:mount_driver_lease=0;break;
 case 19:trust_error=SF_BAD_SIGNATURE;break;case 20:uncertain_commit=1;ledger.modules[fs_slot].resources[RES_DIRTY]=1;break;
 case 21:sf_trust.revoked_ids[0][0]++;break;case 22:sf_trust.policy.count++;break;case 23:sf_trust.policy.keys=policy_keys+1;break;
 default:CHECK(kind>=24&&kind<24+RES_COUNT);ledger.modules[fs_slot].resources[kind-24]++;break;
 }
}
static void healthy_tests(void){
 for(unsigned delay=0;delay<4;delay++){
  reset();uint64_t token=begin();snapshot_state();running_polls=delay;reenter=1;
  SfPersistStopStatus before=report(token);CHECK(before.phase==SF_PERSIST_TERMINAL_ACTIVE&&!before.request_id);
  CHECK(!sf_persist_fence_stop(token));SfPersistStopStatus result=report(token);
  CHECK(result.phase==SF_PERSIST_TERMINAL_STOPPED_CLOSED&&result.ack_authenticated&&result.ack_status==0&&result.ack_mode==1);
  CHECK(result.death_confirmed&&result.retired&&result.task_status==0&&result.observations==delay+1);
  CHECK(!result.request_error&&!result.observation_error&&revokes==2&&sends==1&&receives==1);
  CHECK(!sf_persist_ready()&&!persist_ep&&!persist_reply&&persist_pid==12&&!uncertain_commit&&!trust_error);
  no_retry(token);unchanged();cases++;
 }
 reset();uint64_t token=begin();SfPersistStopStatus out;memset(&out,0xa5,sizeof(out));SfPersistStopStatus before=out;
 CHECK(sf_persist_fence_stop(0)==SF_STALE_HANDLE&&sf_persist_fence_stop(token+1)==SF_STALE_HANDLE&&!sends);
 CHECK(sf_persist_fence_stop_status(0,&out)==SF_STALE_HANDLE&&!memcmp(&out,&before,sizeof(out)));
 CHECK(sf_persist_fence_stop_status(token+1,&out)==SF_STALE_HANDLE&&!memcmp(&out,&before,sizeof(out)));
 CHECK(sf_persist_fence_stop_status(token,0)==SF_INVALID&&!sends);
 CHECK(!sf_persist_fence_cancel(token));uint64_t newer=begin();CHECK(newer>token);
 CHECK(sf_persist_fence_stop(token)==SF_STALE_HANDLE&&!sends&&!sf_persist_fence_cancel(newer));cases++;
}
static void precondition_tests(void){
 for(unsigned bad=1;bad<24+RES_COUNT;bad++){
  reset();uint64_t token=begin();mutate(bad);
  SfManager before=ledger;SfNamespace nodes=space;SfRuntimeTrust trust=sf_trust;int unknown=uncertain_commit,error=trust_error;
  CHECK(sf_persist_fence_stop(token)<0&&!sends&&!receives&&!revokes&&sf_persist_fenced());
  CHECK(terminal_stop.phase==SF_PERSIST_TERMINAL_ACTIVE&&!terminal_stop.request_id);
  CHECK(!memcmp(&before,&ledger,sizeof(before))&&!memcmp(&nodes,&space,sizeof(nodes))&&!memcmp(&trust,&sf_trust,sizeof(trust)));
  CHECK(uncertain_commit==unknown&&trust_error==error);cases++;
 }
 reset();uint64_t token=begin();requests=UINT64_MAX;CHECK(sf_persist_fence_stop(token)==SF_NO_SPACE&&!sends&&!terminal_stop.request_id);
 CHECK(!sf_persist_fence_cancel(token));cases++;
 reset();token=begin();clock_error=SF_DENIED;CHECK(sf_persist_fence_stop(token)==SF_BAD_FORMAT&&!sends&&!terminal_stop.request_id);
 clock_error=0;CHECK(!sf_persist_fence_cancel(token));cases++;
 reset();token=begin();clock_ticks=UINT64_MAX/10000000ull;
 CHECK(sf_persist_fence_stop(token)==SF_BAD_FORMAT&&!sends&&!terminal_stop.request_id);clock_ticks=10;CHECK(!sf_persist_fence_cancel(token));cases++;
 reset();token=begin();pre_status=0;CHECK(sf_persist_fence_stop(token)==SF_DEVICE_GONE&&!sends&&!revokes&&sf_persist_fenced());cases++;
}
static void refusal_tests(void){
 for(int code=SF_VERSION_REJECTED;code<0;code++){
  reset();uint64_t token=begin();snapshot_state();semantic=code;final_status=SF_TASK_RUNNING;
  CHECK(sf_persist_fence_stop(token)==code);SfPersistStopStatus result=report(token);
  int safe=code==SF_INVALID||code==SF_DENIED||code==SF_BUSY||code==SF_TIMEOUT||code==SF_STALE_HANDLE;
  CHECK(result.phase==(safe?SF_PERSIST_TERMINAL_REJECTED:SF_PERSIST_TERMINAL_FAILED)&&result.ack_authenticated&&result.ack_status==code&&!result.ack_mode&&!result.retired&&!revokes);
  CHECK(!uncertain_commit&&!trust_error&&!ledger.modules[fs_slot].resources[RES_DIRTY]);
  CHECK(sf_persist_fence_stop(token)==SF_DRAINING&&sends==1&&receives==1);
  if(safe){CHECK(!sf_persist_fence_cancel(token)&&!sf_persist_fenced());CHECK(report(token).phase==SF_PERSIST_TERMINAL_REJECTED);}
  else no_retry(token);
  unchanged();cases++;
 }
 reset();uint64_t token=begin();semantic=SF_BUSY;final_status=0;
 CHECK(sf_persist_fence_stop(token)==SF_DEVICE_GONE&&terminal_stop.phase==SF_PERSIST_TERMINAL_FAILED&&!revokes);
 no_retry(token);cases++;
}
static void malformed_tests(void){
 for(unsigned bad=1;bad<=29;bad++)for(unsigned dead=0;dead<2;dead++){
  reset();uint64_t token=begin();snapshot_state();variant=bad;final_status=dead?0:SF_TASK_RUNNING;freeze_clock=1;
  CHECK(sf_persist_fence_stop(token)==(dead?SF_BAD_FORMAT:SF_TIMEOUT));SfPersistStopStatus result=report(token);
  CHECK(!result.ack_authenticated&&!result.retired&&!revokes&&sends==1&&receives==1&&!uncertain_commit&&!trust_error);
  CHECK(result.phase==(dead?SF_PERSIST_TERMINAL_FAILED:SF_PERSIST_TERMINAL_REQUEST_UNKNOWN));
  CHECK(result.death_confirmed==(int)dead&&result.observations==(dead?1u:512u));
  CHECK(closes==((bad==18||bad==20||bad==21)?1u:0u));no_retry(token);unchanged();cases++;
 }
}
static void transport_tests(void){
 const int64_t errors[]={SF_TIMEOUT,SF_WOULD_BLOCK,SF_DEVICE_GONE,SF_DENIED,1,1ll<<32,-(1ll<<32)};
 for(unsigned kind=0;kind<2;kind++)for(unsigned e=0;e<sizeof(errors)/sizeof(errors[0]);e++){
  reset();uint64_t token=begin();snapshot_state();freeze_clock=1;final_status=SF_TASK_RUNNING;
  if(kind)recv_result=errors[e];else send_result=errors[e];
  CHECK(sf_persist_fence_stop(token)==SF_TIMEOUT);SfPersistStopStatus result=report(token);
  CHECK(result.phase==SF_PERSIST_TERMINAL_REQUEST_UNKNOWN&&!result.ack_authenticated&&!result.retired&&!revokes);
  CHECK(sends==1&&receives==kind&&result.observations==512);
  CHECK(kind?result.receive_result==errors[e]:result.send_result==errors[e]);
  no_retry(token);unchanged();cases++;
 }
 reset();uint64_t token=begin();recv_result=SF_TIMEOUT;final_status=0;
 CHECK(sf_persist_fence_stop(token)==SF_TIMEOUT&&terminal_stop.phase==SF_PERSIST_TERMINAL_FAILED&&terminal_stop.death_confirmed&&!revokes);
 CHECK(!terminal_stop.ack_authenticated&&!terminal_stop.retired);no_retry(token);cases++;
}
static void observation_tests(void){
 const int64_t statuses[]={SF_TASK_RUNNING,1,INT32_MAX,SF_DENIED,SF_NOT_FOUND,-(1ll<<32),SF_TASK_RUNNING+1,INT64_MAX,INT64_MIN};
 for(unsigned i=0;i<sizeof(statuses)/sizeof(statuses[0]);i++){
  reset();uint64_t token=begin();snapshot_state();final_status=statuses[i];freeze_clock=1;
  CHECK(sf_persist_fence_stop(token)==(statuses[i]==SF_TASK_RUNNING?SF_TIMEOUT:SF_DEVICE_GONE));
  SfPersistStopStatus result=report(token);CHECK(result.ack_authenticated&&result.task_status==statuses[i]&&!result.retired&&!revokes);
  CHECK(result.phase==(statuses[i]==SF_TASK_RUNNING?SF_PERSIST_TERMINAL_ACCEPTED_WAIT:SF_PERSIST_TERMINAL_FAILED));
  CHECK(result.death_confirmed==(int)(statuses[i]>0&&statuses[i]<=INT32_MAX));
  no_retry(token);unchanged();cases++;
 }
 reset();uint64_t token=begin();final_status=SF_TASK_RUNNING;
 CHECK(sf_persist_fence_stop(token)==SF_TIMEOUT&&terminal_stop.observations==301&&clock_ticks==310);
 no_retry(token);cases++;
 reset();token=begin();clock_error=0;final_status=SF_TASK_RUNNING;reenter=0;
 /* The transport mutation changes a pinned clock after SEND, not its deadline. */
 mutation=0;clock_ticks=10;CHECK(sf_persist_fence_stop(token)==SF_TIMEOUT);CHECK(terminal_stop.deadline_ns==3100000000ull);cases++;
 for(unsigned bad=30;bad<=31;bad++){
  reset();token=begin();variant=bad;final_status=SF_TASK_RUNNING;
  CHECK(sf_persist_fence_stop(token)==SF_BAD_FORMAT&&terminal_stop.phase==SF_PERSIST_TERMINAL_FAILED);
  CHECK(terminal_stop.ack_authenticated&&terminal_stop.observation_error==SF_BAD_FORMAT&&!terminal_stop.death_confirmed&&!revokes);
  no_retry(token);cases++;
 }
}
static void changed_after_acceptance_tests(void){
 for(unsigned bad=1;bad<24+RES_COUNT;bad++){
  reset();uint64_t token=begin();mutation=bad;
  CHECK(sf_persist_fence_stop(token)<0&&!revokes&&terminal_stop.ack_authenticated&&terminal_stop.death_confirmed);
  CHECK(terminal_stop.phase==SF_PERSIST_TERMINAL_FAILED&&!terminal_stop.retired);
  if(bad==20)CHECK(uncertain_commit&&ledger.modules[fs_slot].resources[RES_DIRTY]==1);
  if(bad==19)CHECK(trust_error==SF_BAD_SIGNATURE);
  no_retry(token);cases++;
 }
 for(unsigned e=0;e<2;e++)for(unsigned at=1;at<=2;at++){
  reset();uint64_t token=begin();snapshot_state();revoke_error=e?1ll<<32:SF_DENIED;revoke_fail_at=at;
  CHECK(sf_persist_fence_stop(token)==(e?SF_BAD_FORMAT:SF_DENIED));
  CHECK(revokes==2&&terminal_stop.phase==SF_PERSIST_TERMINAL_FAILED&&!terminal_stop.retired);
  CHECK(persist_ep==(at==1?0x101u:0u)&&persist_reply==(at==2?0x102u:0u));no_retry(token);unchanged();cases++;
 }
}
int main(void){
 healthy_tests();precondition_tests();refusal_tests();malformed_tests();transport_tests();observation_tests();changed_after_acceptance_tests();
 printf("PERSIST TERMINAL OWNER PASS %u checks; %u cases; E1 actual Root owners, fake kernel/transport; no candidate, guest or full-suite claim\n",checks,cases);
 return 0;
}
