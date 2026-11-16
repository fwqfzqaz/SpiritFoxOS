/* E1 only: actual Root storage/client/manager/namespace admission functions.
 * Kernel identity and one authenticated advisory reply are simulated. No
 * provider termination, signing, durable write or guest claim is made here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LIBSF_H
#include "sf.h"
static int64_t sf_sys(long,uint64_t,uint64_t,uint64_t);
static long sf_send(uint32_t,SfMessage*);
static long sf_recv(uint32_t,SfMessage*,uint64_t);
static long sf_cap_close(uint64_t);
static long sf_cap_derive(uint64_t,uint64_t);
static long sf_send_cap(uint32_t,SfMessage*,uint64_t);
static uint64_t sf_ticks(void);
static void sf_yield(void);
static uint64_t sf_deadline(unsigned ms){return 10000000ull+(uint64_t)ms*1000000;}
static void sf_message(SfMessage*m,unsigned op,uint64_t id){memset(m,0,sizeof(*m));m->h.size=40;m->h.protocol_major=1;m->h.opcode=op;m->h.request_id=id;m->h.payload_bytes=sizeof(*m)-40;m->h.deadline_ns=sf_deadline(3000);}
static void sf_log(const char*s){(void)s;}
static void sf_number(uint64_t n){(void)n;}
static void sf_assert(int,const char*);
#include "../../services/storage.c"
#include "../../services/persist_client.c"
#include "../../services/namespace_state.h"
#include "../../services/namespace_control.h"

static unsigned checks,sends,receives,closes,losses,reenter,mutation,variant;
static unsigned guard_runs,preflight_cases,context_cases,reply_cases;
static int namespace_alive,send_error,recv_error,semantic;
static uint32_t dead_pid;
static uint64_t clock_ticks;
static SfMessage wire;
static SfMessage namespace_wire;
static SfNsControl namespace_control;
static unsigned raw_transport,raw_sends,raw_receives,raw_control_cases,ram_cases;
static uint64_t raw_requests;
static SfManager book,book_before,other_book;
static SfNamespace space,space_before,other_space;
static SfNode guard_nodes[SF_NS_NODE_MAX];
static SfTrustKey policy_keys[2];
static SfRuntimeTrust trust_before;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"PERSIST FENCE FAIL line %d variant%u mutation%u raw%u/%u: %s\n",__LINE__,variant,mutation,raw_sends,raw_receives,#c);exit(1);}}while(0)

static void guarded_calls(void);
static void mutate_context(unsigned);
static void blocked_exchange_tests(void);
static int no_spawn(const char*p,uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e){(void)p;(void)a;(void)b;(void)c;(void)d;(void)e;CHECK(0);return SF_DENIED;}
static int64_t sf_sys(long op,uint64_t a,uint64_t b,uint64_t c){
 (void)b;(void)c;
 if(op==SYS_TASK_STATUS)return a&&a!=dead_pid?SF_TASK_RUNNING:SF_DEVICE_GONE;
 if(op==SYS_PCI_READ)return 0;
 /* Any endpoint allocation/revocation, kill, DMA or other syscall here means
  * an administrative guard was bypassed. */
 CHECK(0);return SF_DENIED;
}
static uint64_t sf_ticks(void){return clock_ticks++;}
static void sf_yield(void){clock_ticks++;}
static void sf_assert(int value,const char*message){(void)message;CHECK(value);}
static long sf_cap_close(uint64_t cap){CHECK(cap==0x777);closes++;return 0;}
static long sf_cap_derive(uint64_t cap,uint64_t rights){(void)cap;(void)rights;CHECK(0);return SF_DENIED;}
static long sf_send_cap(uint32_t ep,SfMessage*m,uint64_t cap){(void)ep;(void)m;(void)cap;CHECK(0);return SF_DENIED;}
int sf_ns_client_healthy(void){return namespace_alive;}
void sf_ns_peer_lost(void){namespace_alive=0;losses++;}
int64_t sf_ns_client_callback(uint32_t from,SfMessage*m){(void)from;(void)m;CHECK(0);return SF_DENIED;}
static long sf_send(uint32_t ep,SfMessage*m){
 if(raw_transport&&ep==fs_service_ep){
  raw_sends++;namespace_wire=*m;namespace_wire.h.flags=1;
  sf_nsr_dispatch(&namespace_control,1,&namespace_wire);
  namespace_wire.h.flags=fs_pid;namespace_wire.h.deadline_ns=0;return 0;
 }
 CHECK(ep==0x101&&m->h.opcode==OP_PERSIST_QUIESCENT);
 CHECK(!m->handle&&!m->offset&&!m->length&&!m->mode&&!m->status&&!m->h.cap_count);
 sends++;CHECK(sends<=1);if(send_error)return send_error;
 wire=*m;wire.h.flags=12;wire.status=semantic;return 0;
}
static long sf_recv(uint32_t ep,SfMessage*m,uint64_t deadline){
 if(raw_transport&&ep==fs_service_reply){raw_receives++;*m=namespace_wire;return fs_pid;}
 CHECK(ep==0x102&&deadline==sf_deadline(10000));receives++;CHECK(receives<=1);
 if(reenter)guarded_calls();
 if(mutation)mutate_context(mutation);
 if(recv_error)return recv_error;
 *m=wire;
 switch(variant){
 case 1:return 13;case 2:m->h.request_id++;break;case 3:m->h.size--;break;
 case 4:m->h.protocol_major++;break;case 5:m->h.protocol_minor++;break;
 case 6:m->h.opcode++;break;case 7:m->h.flags++;break;case 8:m->h.payload_bytes--;break;
 case 9:m->h.deadline_ns++;break;case 10:m->handle=1;break;case 11:m->offset=1;break;
 case 12:m->length=1;break;case 13:m->mode=1;break;case 14:m->status=1;break;
 case 15:m->status=1ll<<32;break;case 16:m->status=-(1ll<<32);break;
 case 17:m->status=SF_VERSION_REJECTED-1;break;
 case 18:m->h.cap_count=1;m->handle=0x777;break;case 19:m->h.cap_count=2;break;
 case 20:m->h.cap_count=1;m->handle=0x777;return 13;
 case 21:m->h.cap_count=1;m->handle=0x777;m->h.request_id++;break;
 }
 return 12;
}
static void reset(void){
 /* Each reset models a new fixture/boot, never a supported way to clear an
  * unknown-completion latch or recover an invalid held token. */
 memset(&admission_fence,0,sizeof(admission_fence));fence_sequence=load_sequence=load_owner=0;
 memset(&book,0,sizeof(book));memset(&other_book,0,sizeof(other_book));
 manager=&book;ns=&space;spawn=no_spawn;
 driver_slot=sfmod_add(manager,"org.spiritfox.virtio.blk",0,0);CHECK(driver_slot>=0);
 fs_slot=sfmod_add(manager,"org.spiritfox.fs.disk",0,0);CHECK(fs_slot>=0);
 CHECK(!sfmod_start(manager,(unsigned)driver_slot)&&!sfmod_start(manager,(unsigned)fs_slot));
 CHECK(!sfmod_acquire(manager,(unsigned)driver_slot,RES_LEASE));
 CHECK(!sfmod_acquire(manager,(unsigned)driver_slot,RES_DMA));
 driver_pid=22;driver_ep=0x201;driver_reply=0x202;fs_pid=21;
 fs_service_ep=0x301;fs_service_reply=0x302;fs_broker_ep=0x303;fs_broker_reply=0x304;
 persist_pid=12;persist_ep=0x101;persist_reply=0x102;
 mounted=writable=write_grant=mount_admission=mount_driver_lease=1;
 capacity=SF_TXN_SECTORS*512ull;namespace_descriptions=0;memset(&fs_transport,0,sizeof(fs_transport));
 requests=0x300000000ull;sequence=0x100000000ull;fs_requests=0x400000000ull;
 uncertain_commit=trust_error=0;namespace_alive=1;dead_pid=0;clock_ticks=0;
 candidate_count=candidate_next=0;disk_read=0;
 sends=receives=closes=losses=reenter=mutation=variant=0;send_error=recv_error=semantic=0;
 memset(&sf_trust,0,sizeof(sf_trust));sf_trust.policy.keys=policy_keys;sf_trust.policy.count=1;
 sf_trust.revoked=1;sf_trust.revoked_ids[0][0]=0x45;
 sf_ns_init(ns,0);sf_storage_namespace_bind(ns);
 const SfMountEntry disk[]={ {"/disk",NODE_DIR,1,0,0},{"/disk/file",NODE_DISK,1,0,4} };
 CHECK(!sf_ns_mount_publish(ns,disk,2,1)&&!project_notes(ns,4));
 CHECK(!sf_ns_create(ns,"/ram",NODE_FILE,0));
 const SfNsBackends bindings={0,notes_read,notes_write,notes_truncate,mount_open,mount_close,0};
 sf_nsr_init(&namespace_control,ns,&bindings);namespace_control.ready=1;
 namespace_control.mask=SF_NS_B_READ|SF_NS_B_WRITE|SF_NS_B_TRUNCATE|SF_NS_B_MOUNT_OPEN|SF_NS_B_MOUNT_CLOSE;
 raw_transport=raw_sends=raw_receives=0;raw_requests=0x900000000ull;
 CHECK(sf_storage_admission_instance()==(((uint64_t)book.modules[fs_slot].generation<<32)|book.modules[driver_slot].generation));
 CHECK(!sf_storage_quiescent()&&!sf_persist_fenced());
}
static void snapshot_state(void){book_before=book;space_before=space;trust_before=sf_trust;}
static void unchanged(void){
 CHECK(!memcmp(&book,&book_before,sizeof(book))&&!memcmp(&space,&space_before,sizeof(space)));
 CHECK(!memcmp(&sf_trust,&trust_before,sizeof(sf_trust))&&!uncertain_commit&&!trust_error&&!losses);
 CHECK(mounted&&writable&&mount_admission&&mount_driver_lease&&namespace_alive&&!namespace_descriptions);
 CHECK(manager==&book&&ns==&space&&spawn==no_spawn&&fs_pid==21&&driver_pid==22);
 CHECK(persist_pid==12&&persist_ep==0x101&&persist_reply==0x102);
}
static void guarded_calls(void){
 CHECK(sf_persist_fenced());unsigned sent=sends,received=receives,old_closes=closes;
 memcpy(guard_nodes,space.nodes,sizeof(guard_nodes));
 uint8_t bytes[512]={0},output[32];memset(output,0xa5,sizeof(output));
 uint64_t untouched=UINT64_MAX;
 CHECK(mount_open(1)==SF_DRAINING&&mount_open(2)==SF_STALE_HANDLE);
 CHECK(sf_ns_open(ns,7,"/","/persist/notes",OPEN_READ)==SF_DRAINING);
 CHECK(sf_ns_open(ns,7,"/","/disk/file",OPEN_READ)==SF_DRAINING);
 int root=sf_ns_open(ns,7,"/","/",OPEN_READ|OPEN_DIRECTORY);CHECK(root>0);
 CHECK(sf_ns_openat(ns,7,(uint32_t)root,"persist/notes",OPEN_READ)==SF_DRAINING);
 CHECK(!sf_ns_close(ns,7,(uint32_t)root));
 CHECK(sf_storage_load("/disk/file",output,sizeof(output))==SF_DRAINING);
 CHECK(sf_storage_namespace_disk_live()==SF_DRAINING&&external_read(0,0,output,1)==SF_DRAINING);
 CHECK(sf_storage_transaction(1)==SF_DRAINING);
 CHECK(sf_storage_sector_write(0,SF_TXN_BANK,bytes)==SF_DRAINING);
 CHECK(sf_storage_flush(0)==SF_DRAINING&&sf_storage_write_range(SF_TXN_BANK,SF_TXN_BANK+1)==SF_DRAINING);
 const unsigned block_ops[]={OP_BLOCK_WRITE,OP_BLOCK_FLUSH,OP_BLOCK_WRITE_RANGE,OP_BLOCK_STOP,OP_BLOCK_DMA_PROBE};
 for(unsigned i=0;i<sizeof(block_ops)/sizeof(*block_ops);i++){SfMessage m;sf_message(&m,block_ops[i],0);CHECK(call(&m)==SF_DRAINING);}
 CHECK(fs_shutdown()==SF_DRAINING&&fs_attach()==SF_DRAINING&&fs_start()==SF_DRAINING);
 CHECK(sf_storage_namespace_start()==SF_DRAINING&&sf_storage_namespace_reset()==SF_DRAINING);
 CHECK(sf_storage_namespace_crash()==SF_DRAINING&&sf_storage_namespace_restore()==SF_DRAINING);
 CHECK(mount_disk_view()==SF_DRAINING&&start_candidate(0,0)==SF_DRAINING);
 CHECK(sf_storage_start()==SF_DRAINING&&sf_storage_restart_next()==SF_DRAINING);
 CHECK(sf_storage_mount()==SF_DRAINING&&sf_storage_unmount()==SF_DRAINING&&sf_storage_stop()==SF_DRAINING);
 CHECK(sf_storage_crash_test()==SF_DRAINING);
 sf_storage_dma_probe(0);sf_storage_write_tests();sf_storage_tests();
 sf_storage_init(&other_book,&other_space,0);sf_storage_namespace_bind(&other_space);
 CHECK(manager==&book&&ns==&space&&spawn==no_spawn);
 CHECK(sf_persist_start(no_spawn)==SF_DRAINING&&attach()==SF_DRAINING&&sf_persist_mount(ns)==SF_DRAINING);
 sf_persist_attach();sf_persist_driver_down();sf_persist_status();sf_persist_trust_status();
 CHECK(trust_sync(0)==SF_DRAINING&&trust_sync(1)==SF_DRAINING);
 CHECK(sf_persist_notes_size(&untouched)==SF_DRAINING&&untouched==UINT64_MAX);
 CHECK(sf_persist_reproject(ns)==SF_DRAINING&&sf_persist_begin()==SF_DRAINING&&sf_persist_finish(0)==SF_DRAINING);
 CHECK(sf_persist_install("/disk/file")==SF_DRAINING&&sf_persist_revoke("/disk/file")==SF_DRAINING);
 CHECK(sf_persist_package(output,sizeof(output))==SF_DRAINING&&sf_persist_preview(output,sizeof(output))==SF_DRAINING);
 CHECK(notes_read(UINT32_MAX,0,output,1)==SF_DRAINING&&notes_write(UINT32_MAX,0,bytes,1)==SF_DRAINING);
 CHECK(notes_truncate(UINT32_MAX,0)==SF_DRAINING&&stage(bytes,1)==SF_DRAINING);
 const unsigned persist_ops[]={OP_PERSIST_MOUNT,OP_PERSIST_READ,OP_PERSIST_WRITE,OP_PERSIST_TRUNCATE,OP_PERSIST_STAGE,OP_PERSIST_INSTALL,OP_PERSIST_BEGIN,OP_PERSIST_FINISH,OP_PERSIST_REVOKE,OP_PERSIST_STATUS,OP_PERSIST_PACKAGE,OP_PERSIST_PREVIEW,OP_PERSIST_NOTES_SIZE,OP_PERSIST_REVOKED,OP_PERSIST_DRIVER_DOWN};
 for(unsigned i=0;i<sizeof(persist_ops)/sizeof(*persist_ops);i++){SfMessage m;sf_message(&m,persist_ops[i],0);CHECK(pf_call(&m)==SF_DRAINING);}
 CHECK(pf_send_cap(OP_PERSIST_ATTACH_SEND,0x777,22,0)==SF_DRAINING&&closes==old_closes+1);
 CHECK(sf_persist_load_begin(&untouched)==SF_DRAINING&&untouched==UINT64_MAX);
 blocked_exchange_tests();
 CHECK(sends==sent&&receives==received&&!load_owner&&!namespace_descriptions&&!book.modules[fs_slot].resources[RES_DIRTY]);
 CHECK(!memcmp(guard_nodes,space.nodes,sizeof(guard_nodes)));
 for(unsigned i=0;i<sizeof(output);i++)CHECK(output[i]==0xa5);
 guard_runs++;
}
static void raw_message(SfMessage*m,unsigned op){sf_message(m,op,++raw_requests);m->h.deadline_ns=0;m->h.flags=1;}
static void raw_path(SfMessage*m,uint32_t owner,const char*a,const char*b,uint32_t value){
 const uint32_t fields[]={owner,0,0,value};memcpy(m->data,fields,sizeof(fields));
 size_t n=strlen(a)+1;memcpy(m->data+16,a,n);m->length=(uint32_t)(16+n);
 if(b){n=strlen(b)+1;memcpy(m->data+m->length,b,n);m->length+=(uint32_t)n;}
}
static void blocked_exchange_tests(void){
 const unsigned controls[]={OP_NS_UNMOUNT,OP_NS_INIT,OP_NS_BACKENDS,OP_NS_ASSIGN,OP_NS_PROJECT,OP_NS_PROTECT,
  OP_NS_MOUNT_BEGIN,OP_NS_MOUNT_ENTRY,OP_NS_MOUNT_COMMIT,OP_NS_MOUNT_ABORT,
  OP_FS_ATTACH_SEND,OP_FS_ATTACH_REPLY,OP_FS_MOUNT,OP_FS_STOP,OP_FS_ENTRY,OP_FS_FIND,OP_FS_READ,OP_FS_BLOCK_READ,0xffff};
 unsigned transport_before=raw_transport,sent=raw_sends,received=raw_receives,closed=closes;raw_transport=1;
 SfNamespace before=space;SfNsControl control_before=namespace_control;SfNsTransport transport=fs_transport;SfManager ledger=book;
 for(unsigned i=0;i<sizeof(controls)/sizeof(*controls);i++){
  SfMessage m;raw_message(&m,controls[i]);
  if(m.h.opcode==OP_NS_UNMOUNT)m.mode=1; /* Valid tag1 destructive reproducer. */
  if(m.h.opcode==OP_NS_INIT||m.h.opcode==OP_NS_BACKENDS)m.mode=0;
  if(m.h.opcode==OP_NS_ASSIGN)raw_path(&m,0,"/persist/notes",0,0777);
  if(m.h.opcode==OP_NS_PROJECT){m.mode=17;raw_path(&m,0,"/new-projection",0,0);}
  if(m.h.opcode==OP_NS_PROTECT)raw_path(&m,0,"/",0,0);
  if(m.h.opcode==OP_NS_MOUNT_BEGIN){m.handle=1;m.mode=1;}
  if(m.h.opcode==OP_FS_ATTACH_SEND||m.h.opcode==OP_FS_ATTACH_REPLY){m.h.cap_count=1;m.handle=0x777;}
  SfMessage original=m;
  CHECK(sf_storage_namespace_exchange(0,&m)==SF_DRAINING&&!memcmp(&m,&original,sizeof(m)));
  CHECK(raw_sends==sent&&raw_receives==received&&closes==closed&&!memcmp(&transport,&fs_transport,sizeof(transport)));
  CHECK(!memcmp(&before,&space,sizeof(before))&&!memcmp(&control_before,&namespace_control,sizeof(control_before)));
  CHECK(!memcmp(&ledger,&book,sizeof(ledger))&&sf_persist_fenced());raw_control_cases++;
 }
 for(unsigned type=NODE_DISK;type<=NODE_PROJECTION;type++){
  if(type!=NODE_DISK&&type!=NODE_PROJECTION)continue;
  SfMessage m;raw_message(&m,OP_NS_CREATE);m.mode=type;raw_path(&m,0,"/backend-alias",0,0600);SfMessage original=m;
  CHECK(sf_storage_namespace_exchange(0,&m)==SF_DRAINING&&!memcmp(&m,&original,sizeof(m)));
  CHECK(raw_sends==sent&&raw_receives==received&&closes==closed&&!memcmp(&transport,&fs_transport,sizeof(transport)));
  CHECK(!memcmp(&before,&space,sizeof(before))&&!memcmp(&control_before,&namespace_control,sizeof(control_before)));
  CHECK(!memcmp(&ledger,&book,sizeof(ledger))&&sf_persist_fenced());raw_control_cases++;
 }
 CHECK(sf_storage_namespace_exchange(0,0)==SF_INVALID);
 raw_transport=transport_before;
}
static void raw_ok(SfMessage*m){
 unsigned sent=raw_sends,received=raw_receives;
 CHECK(sf_storage_namespace_exchange(0,m)==fs_pid&&raw_sends==sent+1&&raw_receives==received+1);
 CHECK(m->status>=0&&namespace_alive&&!fs_transport.inflight);ram_cases++;
}
static void ordinary_ram_tests(void){
 reset();CHECK(!sf_ns_create(ns,"/preexisting-disk",NODE_DISK,0));
 uint64_t token=0;CHECK(!sf_persist_fence_begin(&token));raw_transport=1;
 SfNode tagged[SF_NS_NODE_MAX];memcpy(tagged,space.nodes,sizeof(tagged));SfManager ledger=book;SfRuntimeTrust trust=sf_trust;
 SfNsBackends bindings=namespace_control.backends;uint32_t mask=namespace_control.mask;
 SfMessage m;raw_message(&m,OP_NS_CREATE);m.mode=NODE_FILE;raw_path(&m,0,"/fenced-ram",0,0600);raw_ok(&m);
 raw_message(&m,OP_NS_OPEN);m.mode=OPEN_READ|OPEN_WRITE;raw_path(&m,7,"/","/fenced-ram",0);raw_ok(&m);uint32_t file=(uint32_t)m.handle;CHECK(file);
 raw_message(&m,OP_NS_WRITE);m.mode=7;m.handle=file;m.length=3;memcpy(m.data,"ram",3);raw_ok(&m);CHECK(m.status==3);
 raw_message(&m,OP_NS_PREAD);m.mode=7;m.handle=file;m.offset=1;m.length=2;raw_ok(&m);CHECK(m.status==2&&!memcmp(m.data,"am",2));
 raw_message(&m,OP_NS_SEEK);m.mode=7;m.handle=file;raw_ok(&m);
 raw_message(&m,OP_NS_READ);m.mode=7;m.handle=file;m.length=3;raw_ok(&m);CHECK(m.status==3&&!memcmp(m.data,"ram",3));
 raw_message(&m,OP_NS_STAT);m.mode=7;m.handle=file;raw_ok(&m);SfFileStat info;memcpy(&info,m.data,sizeof(info));CHECK(info.size==3);
 raw_message(&m,OP_NS_DUP);m.mode=7;m.handle=file;raw_ok(&m);uint32_t copy=(uint32_t)m.handle;CHECK(copy&&copy!=file);
 raw_message(&m,OP_NS_HANDOFF);m.mode=7;m.handle=copy;m.offset=8;raw_ok(&m);
 raw_message(&m,OP_NS_REAP);m.mode=8;raw_ok(&m);
 raw_message(&m,OP_NS_TRUNCATE);m.mode=7;m.handle=file;m.offset=1;raw_ok(&m);
 raw_message(&m,OP_NS_POLL);m.mode=7;m.handle=file;raw_ok(&m);
 raw_message(&m,OP_NS_CLOSE);m.mode=7;m.handle=file;raw_ok(&m);
 raw_message(&m,OP_NS_OPEN);m.mode=OPEN_READ|OPEN_WRITE|OPEN_DIRECTORY;raw_path(&m,7,"/","/",0);raw_ok(&m);uint32_t directory=(uint32_t)m.handle;CHECK(directory);
 raw_message(&m,OP_NS_ENUMERATE);m.mode=7;m.handle=directory;m.length=sizeof(m.data);raw_ok(&m);
 raw_message(&m,OP_NS_ENUMERATE_AT);m.mode=7;m.handle=directory;m.length=sizeof(m.data);raw_ok(&m);
 raw_message(&m,OP_NS_UNLINK);m.handle=directory;raw_path(&m,7,"fenced-ram",0,0);raw_ok(&m);
 raw_message(&m,OP_NS_CREATE);m.mode=NODE_DIR;raw_path(&m,0,"/fenced-dir",0,0700);raw_ok(&m);
 raw_message(&m,OP_NS_RMDIR);m.handle=directory;raw_path(&m,7,"fenced-dir",0,0);raw_ok(&m);
 raw_message(&m,OP_NS_CREATE);m.mode=NODE_PIPE;raw_path(&m,0,"/fenced-pipe",0,0600);raw_ok(&m);
 raw_message(&m,OP_NS_OPEN);m.mode=OPEN_READ|OPEN_WRITE;raw_path(&m,7,"/","/fenced-pipe",0);raw_ok(&m);uint32_t pipe=(uint32_t)m.handle;CHECK(pipe);
 raw_message(&m,OP_NS_WRITE);m.mode=7;m.handle=pipe;m.length=4;memcpy(m.data,"pipe",4);raw_ok(&m);CHECK(m.status==4);
 raw_message(&m,OP_NS_READ);m.mode=7;m.handle=pipe;m.length=4;raw_ok(&m);CHECK(m.status==4&&!memcmp(m.data,"pipe",4));
 raw_message(&m,OP_NS_CLOSE);m.mode=7;m.handle=pipe;raw_ok(&m);
 raw_message(&m,OP_NS_CREATE);m.mode=NODE_CONSOLE;raw_path(&m,0,"/fenced-console",0,0600);raw_ok(&m);
 raw_message(&m,OP_NS_OPEN);m.mode=OPEN_READ|OPEN_WRITE;raw_path(&m,7,"/","/fenced-console",0);raw_ok(&m);uint32_t console=(uint32_t)m.handle;CHECK(console);
 raw_message(&m,OP_NS_STAT);m.mode=7;m.handle=console;raw_ok(&m);memcpy(&info,m.data,sizeof(info));CHECK(info.type==NODE_CONSOLE);
 raw_message(&m,OP_NS_CLOSE);m.mode=7;m.handle=console;raw_ok(&m);
 raw_message(&m,OP_NS_MOUNT_CHECK);raw_path(&m,0,"/unused-mount",0,0);raw_ok(&m);
 raw_message(&m,OP_NS_OPEN);m.mode=OPEN_READ;raw_path(&m,7,"/","/persist/notes",0);
 CHECK(sf_storage_namespace_exchange(0,&m)==fs_pid&&m.status==SF_DRAINING&&namespace_alive&&!namespace_descriptions);
 raw_message(&m,OP_NS_OPEN);m.mode=OPEN_READ;raw_path(&m,7,"/","/preexisting-disk",0);raw_ok(&m);uint32_t alias=(uint32_t)m.handle;CHECK(alias);
 raw_message(&m,OP_NS_READ);m.mode=7;m.handle=alias;m.length=1;
 CHECK(sf_storage_namespace_exchange(0,&m)==fs_pid&&m.status==SF_DRAINING&&namespace_alive&&!namespace_descriptions);
 raw_message(&m,OP_NS_CLOSE);m.mode=7;m.handle=alias;raw_ok(&m);
 raw_message(&m,OP_NS_REAP);m.mode=7;raw_ok(&m);
 CHECK(!memcmp(&ledger,&book,sizeof(ledger))&&!memcmp(&trust,&sf_trust,sizeof(trust)));
 CHECK(!memcmp(&bindings,&namespace_control.backends,sizeof(bindings))&&mask==namespace_control.mask);
 CHECK(space.external_read==notes_read&&space.external_write==notes_write&&space.external_truncate==notes_truncate&&space.mount_open==mount_open&&space.mount_close==mount_close);
 for(unsigned i=0;i<SF_NS_NODE_MAX;i++)if(tagged[i].mount_tag)CHECK(!memcmp(&tagged[i],&space.nodes[i],sizeof(tagged[i])));
 CHECK(!sf_persist_fence_cancel(token)&&!sf_persist_fenced()&&!namespace_descriptions&&!book.modules[fs_slot].resources[RES_OPEN]);
 /* The real peer applies these controls outside a fence. These positive
  * controls make the destructive denied cases more than a no-op transport. */
 raw_message(&m,OP_NS_BACKENDS);raw_ok(&m);CHECK(!space.mount_open&&!space.mount_close&&!space.external_read&&!namespace_control.mask);
 raw_message(&m,OP_NS_UNMOUNT);m.mode=1;raw_ok(&m);CHECK(sf_ns_open(ns,7,"/","/persist/notes",OPEN_READ)==SF_NOT_FOUND);
}
static void healthy_tests(void){
 reset();snapshot_state();uint64_t token=0;
 reenter=1;CHECK(!sf_persist_fence_begin(&token)&&token&&sf_persist_fenced()&&guard_runs);
 CHECK(sends==1&&receives==1);reenter=0;
 /* The root-directory handle used for openat legitimately changed only its
  * generation; take a fresh namespace snapshot after that complete close. */
 space_before=space;unchanged();guarded_calls();space_before=space;unchanged();
 uint64_t nested=0x1234;CHECK(sf_persist_fence_begin(&nested)==SF_BUSY&&nested==0x1234);
 CHECK(sf_persist_fence_cancel(0)==SF_STALE_HANDLE&&sf_persist_fence_cancel(token+1)==SF_STALE_HANDLE&&sf_persist_fenced());
 CHECK(!sf_persist_fence_cancel(token)&&!sf_persist_fenced());unchanged();
 CHECK(sf_persist_fence_cancel(token)==SF_STALE_HANDLE);
 sends=receives=0;uint64_t newer=0;CHECK(!sf_persist_fence_begin(&newer)&&newer>token);
 CHECK(sf_persist_fence_cancel(token)==SF_STALE_HANDLE&&sf_persist_fenced());CHECK(!sf_persist_fence_cancel(newer));
 int h=sf_ns_open(ns,7,"/","/persist/notes",OPEN_READ);CHECK(h>0&&!sf_ns_close(ns,7,(uint32_t)h));
 CHECK(!sf_storage_transaction(1)&&!sf_storage_transaction(0));
}
static void preflight_tests(void){
 uint64_t token;
 CHECK(sf_persist_fence_begin(0)==SF_INVALID&&sf_persist_load_begin(0)==SF_INVALID);
 for(unsigned resource=0;resource<RES_COUNT;resource++){
  reset();CHECK(!sfmod_acquire(manager,(unsigned)fs_slot,resource));snapshot_state();token=9;
  CHECK(sf_persist_fence_begin(&token)==SF_BUSY&&!token&&!sf_persist_fenced()&&!sends);
  CHECK(!memcmp(&book,&book_before,sizeof(book))&&!memcmp(&sf_trust,&trust_before,sizeof(sf_trust)));preflight_cases++;
 }
 const char*paths[]={"/persist/notes","/disk/file","/persist","/disk"};
 for(unsigned i=0;i<sizeof(paths)/sizeof(*paths);i++){
  reset();int h=sf_ns_open(ns,7,"/",paths[i],OPEN_READ);CHECK(h>0);int duplicate=sf_ns_dup(ns,7,(uint32_t)h);CHECK(duplicate>0);
  CHECK(namespace_descriptions==1&&book.modules[fs_slot].resources[RES_OPEN]==1);
  token=9;CHECK(sf_persist_fence_begin(&token)==SF_BUSY&&!token&&!sends);
  CHECK(!sf_ns_close(ns,7,(uint32_t)h));token=9;CHECK(sf_persist_fence_begin(&token)==SF_BUSY&&!token&&!sends);
  sf_ns_reap(ns,7);CHECK(!namespace_descriptions&&!book.modules[fs_slot].resources[RES_OPEN]);
  CHECK(!sf_persist_fence_begin(&token)&&!sf_persist_fence_cancel(token));preflight_cases++;
 }
 /* A different but healthy identity/trust cache may establish a new fence.
  * Only unavailable, busy or fail-closed contexts refuse initial admission. */
 const unsigned unavailable[]={9,12,13,15,16,17,18,19,20,21,22,23,24,25,26,27};
 for(unsigned i=0;i<sizeof(unavailable)/sizeof(*unavailable);i++){
  reset();mutate_context(unavailable[i]);token=9;CHECK(sf_persist_fence_begin(&token)<0&&!token&&!sf_persist_fenced()&&!sends);preflight_cases++;
 }
 reset();fs_transport.inflight=1;token=9;CHECK(sf_persist_fence_begin(&token)==SF_BUSY&&!token&&!sends);
 reset();fence_sequence=UINT64_MAX;token=9;CHECK(sf_persist_fence_begin(&token)==SF_NO_SPACE&&!token&&!sf_persist_fenced()&&!sends);
}
static void load_tests(void){
 reset();uint64_t first=0,second=0,token=9;
 CHECK(!sf_persist_load_begin(&first)&&first&&load_owner==first);
 CHECK(sf_persist_load_begin(&second)==SF_BUSY&&!second);
 CHECK(sf_persist_fence_begin(&token)==SF_BUSY&&!token&&!sends);
 CHECK(sf_persist_load_end(0)==SF_STALE_HANDLE&&sf_persist_load_end(first+1)==SF_STALE_HANDLE&&load_owner==first);
 CHECK(!sf_persist_load_end(first)&&!load_owner&&sf_persist_load_end(first)==SF_STALE_HANDLE);
 CHECK(!sf_persist_load_begin(&second)&&second>first);
 CHECK(sf_persist_load_end(first)==SF_STALE_HANDLE&&load_owner==second);CHECK(!sf_persist_load_end(second));
 load_sequence=UINT64_MAX;CHECK(sf_persist_load_begin(&token)==SF_NO_SPACE&&!load_owner);
}
static void reply_tests(void){
 for(unsigned bad=1;bad<=21;bad++)for(unsigned busy=0;busy<2;busy++){
  reset();variant=bad;semantic=busy?SF_BUSY:0;snapshot_state();uint64_t token=9;
  int expected=bad<=2||bad>=20?SF_STALE_HANDLE:SF_BAD_FORMAT;
  CHECK(sf_persist_fence_begin(&token)==expected&&!token&&!sf_persist_fenced());
  CHECK(sends==1&&receives==1&&closes==(bad==18||bad==20||bad==21));unchanged();reply_cases++;
 }
 for(int error=SF_INVALID;error>=SF_VERSION_REJECTED;error--){
  reset();semantic=error;snapshot_state();uint64_t token=9;
  CHECK(sf_persist_fence_begin(&token)==error&&!token&&!sf_persist_fenced());unchanged();reply_cases++;
 }
 reset();send_error=SF_WOULD_BLOCK;snapshot_state();uint64_t token=9;
 CHECK(sf_persist_fence_begin(&token)==SF_WOULD_BLOCK&&!token&&!receives&&!sf_persist_fenced());unchanged();
 reset();recv_error=SF_TIMEOUT;snapshot_state();token=9;
 CHECK(sf_persist_fence_begin(&token)==SF_TIMEOUT&&!token&&!sf_persist_fenced());unchanged();
}
static void mutate_context(unsigned bad){
 switch(bad){
 case 1:persist_pid++;break;case 2:persist_ep++;break;case 3:persist_reply++;break;
 case 4:fs_pid++;break;case 5:driver_pid++;break;case 6:driver_ep++;break;case 7:driver_reply++;break;
 case 8:capacity+=512;break;case 9:writable=0;break;
 case 10:book.modules[fs_slot].generation++;break;case 11:book.modules[driver_slot].generation++;break;
 case 12:trust_error=SF_BAD_FORMAT;break;
 case 13:uncertain_commit=1;book.modules[fs_slot].resources[RES_DIRTY]=1;break;
 case 14:sf_trust.revoked_ids[0][1]^=1;break;case 15:namespace_alive=0;break;
 case 16:mount_admission=0;break;case 17:mount_driver_lease=0;break;
 case 18:book.modules[driver_slot].resources[RES_LEASE]=0;break;
 case 19:book.modules[fs_slot].state=MOD_DRAINING;break;case 20:book.modules[driver_slot].state=MOD_DRAINING;break;
 case 21:dead_pid=persist_pid;break;case 22:dead_pid=fs_pid;break;case 23:dead_pid=driver_pid;break;
 case 24:mounted=0;break;case 25:fs_transport.inflight=1;break;
 case 26:namespace_descriptions=book.modules[fs_slot].resources[RES_OPEN]=1;break;
 case 27:book.modules[fs_slot].resources[RES_DIRTY]=1;break;
 case 28:sf_trust.policy.count++;break;case 29:sf_trust.policy.keys=policy_keys+1;break;
 case 30:sf_trust.revoked++;break;
 default:CHECK(0);
 }
}
static void changed_context_tests(void){
 for(unsigned bad=1;bad<=30;bad++){
  reset();uint64_t token=0;CHECK(!sf_persist_fence_begin(&token)&&token);
  mutate_context(bad);SfRuntimeTrust changed=sf_trust;int unknown=uncertain_commit,error=trust_error;
  unsigned dirty=book.modules[fs_slot].resources[RES_DIRTY];
  CHECK(sf_persist_fence_cancel(token)<0&&sf_persist_fenced()&&admission_fence.token==token);
  CHECK(!memcmp(&sf_trust,&changed,sizeof(changed))&&uncertain_commit==unknown&&trust_error==error);
  CHECK(book.modules[fs_slot].resources[RES_DIRTY]==dirty&&sends==1&&receives==1);
  CHECK(sf_storage_start()==SF_DRAINING&&mount_open(1)==SF_DRAINING);context_cases++;
  reset();mutation=bad;token=0;CHECK(sf_persist_fence_begin(&token)<0&&token&&sf_persist_fenced());
  CHECK(admission_fence.token==token&&sf_persist_fence_cancel(token)<0&&sends==1&&receives==1);context_cases++;
 }
}
static void raw_unmount_reproducer(void){
 reset();uint64_t token=0;CHECK(!sf_persist_fence_begin(&token));raw_transport=1;
 SfNamespace before=space;SfNsControl control=namespace_control;SfNsTransport transport=fs_transport;SfManager ledger=book;SfRuntimeTrust trust=sf_trust;
 SfMessage m;raw_message(&m,OP_NS_UNMOUNT);m.mode=1;SfMessage original=m;
 CHECK(sf_storage_namespace_exchange(0,&m)==SF_DRAINING&&raw_sends==0&&raw_receives==0);
 CHECK(!memcmp(&m,&original,sizeof(m))&&!memcmp(&transport,&fs_transport,sizeof(transport)));
 CHECK(!memcmp(&before,&space,sizeof(before))&&!memcmp(&control,&namespace_control,sizeof(control)));
 CHECK(!memcmp(&ledger,&book,sizeof(ledger))&&!memcmp(&trust,&sf_trust,sizeof(trust)));
 CHECK(!sf_persist_fence_cancel(token)&&!sf_persist_fenced());raw_control_cases++;
}
int main(int argc,char**argv){
 if(argc==2&&!strcmp(argv[1],"--raw-unmount-only")){raw_unmount_reproducer();printf("RAW UNMOUNT FENCE PASS %u checks; no control IPC or state changes\n",checks);return 0;}
 CHECK(argc==1);healthy_tests();preflight_tests();load_tests();reply_tests();changed_context_tests();ordinary_ram_tests();raw_unmount_reproducer();
 printf("PERSIST FENCE PASS %u checks; %u guarded passes, %u local refusals, %u reply cases, %u changed contexts; %u raw controls refused, %u actual peer RAM/control operations; E1 actual Root admission only\n",checks,guard_runs,preflight_cases,reply_cases,context_cases,raw_control_cases,ram_cases);
 return 0;
}
