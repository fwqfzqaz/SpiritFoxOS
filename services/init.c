#include "libsf.h"
#include "package.h"
#include "manager.h"
#include "namespace.h"
#include "storage.h"
#include "persistence.h"
#include "trust.h"
#include "arguments.h"
#include "native_image.h"
#include "file_rpc.h"
static const SfBootArgs*boot;
static uint32_t manager_ep;static SfManager manager;static SfNamespace ns;
static int runtime_slot,echo_slot;static uint32_t runtime_pid,runtime_ep,runtime_reply,echo_pid,echo_ep;
static uint8_t staging[1024*1024] __attribute__((aligned(4096)));
static unsigned pie_sequence;
static uint8_t package_snapshot[SF_READFS_FILE_MAX];
typedef struct {uint32_t pid,reply,root;int lease;} Child;
static Child children[16];static uint64_t requests;
typedef struct {uint32_t pid,service,reply;} EchoPeer;
static EchoPeer echo_peers[16];
static long console_write(const void*p,size_t n){return sf_sys(SYS_LOG,(uint64_t)p,n,0);}
static int package_get(const char*name,SfPackage*p){const uint8_t*d;uint64_t n;if(name[0]=='/'){long r=!strcmp(name,"/installed.sfk")?sf_persist_package(package_snapshot,sizeof(package_snapshot)):sf_storage_load(name,package_snapshot,sizeof(package_snapshot));return r<0?r:sf_package_open_trusted(p,package_snapshot,r,&sf_trust_store);}int r=sf_bundle_find((void*)boot->bundle,boot->bundle_bytes,name,&d,&n);return r?r:sf_package_open_trusted(p,d,n,&sf_trust_store);}
static int native_upload(const SfPackage*p){for(uint64_t off=0;off<p->bytes;){SfMessage m;sf_message(&m,OP_NATIVE_STAGE,++requests);m.handle=p->bytes;m.offset=off;m.length=p->bytes-off;if(m.length>sizeof(m.data))m.length=sizeof(m.data);uint32_t sent=m.length;memcpy(m.data,p->data+off,sent);int rc=sf_send(runtime_ep,&m);if(rc)return rc;long from=sf_recv(runtime_reply,&m,sf_deadline(1000));if(from!=(long)runtime_pid||m.h.request_id!=requests)return from<0?from:SF_STALE_HANDLE;if(m.status)return m.status;off+=sent;if(m.offset!=off)return SF_BAD_FORMAT;}return 0;}
static int spawn_package_policy(const char*name,uint32_t service,uint32_t rights,uint32_t device,uint32_t device_rights,int directory,const SfArguments*arguments,uint32_t reply,uint16_t expected_kind){
 SfPackage p;int r=package_get(name,&p);if(r)return r;if(p.kind!=expected_kind)return SF_DENIED;
 int native_module=p.kind==1&&!strcmp(name,"sf-native.sfmod");
 if(arguments&&(arguments->count||arguments->env_count)&&p.kind!=2)return SF_DENIED;
 uint64_t image_base=p.native_profile?SF_PIE_MIN+((pie_sequence++&1)*0x2000000ull):SF_BASE;
 if(directory){r=sf_manifest_capability(&p,"namespace.read");if(r<0)return r;if(p.kind!=2||r!=1)return SF_DENIED;}
 if(p.kind==2){char id[96];if(sf_manifest_text(&p,"runtime_id",id,sizeof(id))||strcmp(id,"org.spiritfox.native")||manager.modules[runtime_slot].state!=MOD_ACTIVE)return SF_DEPENDENCY;
  if(sf_sys(SYS_TASK_STATUS,runtime_pid,0,0)!=SF_TASK_RUNNING)return SF_DEVICE_GONE;
  int external=name[0]=='/';if(external){int status=native_upload(&p);if(status)return status;}
  SfMessage request;sf_message(&request,OP_NATIVE_PREPARE,++requests);request.length=strlen(name)+1;if(request.length>sizeof(request.data))return SF_INVALID;request.mode=external;request.offset=image_base;memcpy(request.data,name,request.length);int rc=sf_send(runtime_ep,&request);if(rc)return rc;long from=sf_recv(runtime_reply,&request,sf_deadline(1000));if(from!=(long)runtime_pid||request.h.request_id!=requests)return from<0?from:SF_INVALID;if(request.status)return request.status;
  if(request.length!=sizeof(SfNativePlan))return SF_BAD_FORMAT;SfNativePlan native;memcpy(&native,request.data,sizeof(native));if(native.entry!=p.entry||native.image_base!=image_base||native.profile!=p.native_profile||native.relocation_offset!=p.relocation_offset||native.relocation_count!=p.relocation_count||native.reserved||native.count>5)return SF_BAD_FORMAT;unsigned j=0;for(unsigned i=0;i<p.count;i++){SfSection*s=&p.sections[i];if(s->type>3)continue;if(j>=native.count)return SF_BAD_FORMAT;SfNativeSegment*t=&native.segments[j++];if(t->rva!=s->rva||t->memory!=s->memory||t->offset!=s->offset||t->bytes!=s->bytes||t->flags!=s->flags||t->reserved)return SF_BAD_FORMAT;}if(j!=native.count)return SF_BAD_FORMAT;
 }
 SfLoadPlan plan={0};plan.size=sizeof(plan);plan.entry=p.entry;plan.grant_manager=reply?reply:native_module?runtime_reply:manager_ep;plan.grant_service=service;plan.service_rights=rights;plan.grant_bundle=native_module;
 plan.grant_device=device;plan.device_rights=device_rights;
 if(arguments)plan.arguments=*arguments;
 r=sf_native_image(&p,image_base,staging,sizeof(staging),&plan);if(r)return r;
 return sf_sys(SYS_SPAWN,(uint64_t)&plan,0,0);
}
static int spawn_package_device(const char*name,uint32_t service,uint32_t rights,uint32_t device,uint32_t device_rights){return spawn_package_policy(name,service,rights,device,device_rights,0,0,0,1);}
static int spawn_storage_package(const char*name,uint32_t service,uint32_t rights,uint32_t device,uint32_t device_rights,uint32_t reply){return spawn_package_policy(name,service,rights,device,device_rights,0,0,reply,1);}
static int spawn_package(const char*name,uint32_t service,uint32_t rights){return spawn_package_device(name,service,rights,0,0);}
static EchoPeer*echo_peer(uint32_t pid){for(unsigned i=0;i<16;i++)if(echo_peers[i].pid==pid)return &echo_peers[i];return 0;}
static int spawn_echo(const char*name,uint32_t service,uint32_t rights){
 unsigned slot;for(slot=0;slot<16;slot++)if(!echo_peers[slot].pid)break;if(slot==16)return SF_NO_SPACE;
 int reply=sf_sys(SYS_EP_CREATE,0,0,0);if(reply<0)return reply;int pid=spawn_package_policy(name,service,rights,0,0,0,0,reply,1);
 if(pid<0){sf_sys(SYS_CAP_REVOKE,reply,0,0);return pid;}echo_peers[slot]=(EchoPeer){pid,service,reply};return pid;
}
static void echo_forget(uint32_t pid){EchoPeer*p=echo_peer(pid);if(p){sf_sys(SYS_CAP_REVOKE,p->reply,0,0);memset(p,0,sizeof(*p));}}
static int echo_start_service(void){
 int r=sfmod_start(&manager,echo_slot);if(r)return r;int ep=sf_sys(SYS_EP_CREATE,0,0,0);if(ep<0){r=ep;goto fail;}
 int pid=spawn_echo("echo.sfmod",ep,RIGHT_RECV);if(pid<0){r=pid;sf_sys(SYS_CAP_REVOKE,ep,0,0);goto fail;}
 echo_ep=ep;echo_pid=pid;return 0;
fail:sfmod_prepare(&manager,echo_slot);sfmod_stop(&manager,echo_slot);echo_ep=echo_pid=0;return r;
}
static int echo_stop_service(void){
 int r=sfmod_prepare(&manager,echo_slot);if(!r)r=sfmod_stop(&manager,echo_slot);if(r){sfmod_cancel(&manager,echo_slot);return r;}
 sf_sys(SYS_KILL,echo_pid,0,0);echo_forget(echo_pid);sf_sys(SYS_CAP_REVOKE,echo_ep,0,0);echo_ep=echo_pid=0;return 0;
}
static Child*child(uint32_t pid){for(unsigned i=0;i<16;i++)if(children[i].pid==pid)return&children[i];return 0;}
static int launch_app_root_args(const char*name,const char*root,const SfArguments*arguments){
 if(!strcmp(name,"installed"))name="/installed.sfk";
 int slot=-1;for(unsigned i=0;i<16;i++)if(!children[i].pid){slot=i;break;}if(slot<0)return SF_NO_SPACE;
 /* Reserve and pin the directory before creating a child. No failure after
  * spawn can leave an unregistered child's RPC in the manager queue. */
 int directory=root?sf_ns_open(&ns,1,"/",root,OPEN_READ|OPEN_DIRECTORY):0;if(directory<0)return directory;
 int ep=0,r=sfmod_acquire(&manager,runtime_slot,RES_LEASE);if(r)goto fail_root;
 ep=sf_sys(SYS_EP_CREATE,0,0,0);if(ep<0){r=ep;goto fail_lease;}
 int pid=spawn_package_policy(name,ep,RIGHT_RECV,0,0,directory!=0,arguments,0,2);if(pid<0){r=pid;goto fail_lease;}
 if(directory)ns.handles[(directory&255)-1].owner=pid;
 children[slot]=(Child){pid,ep,directory,1};return pid;
fail_lease:if(ep>0)sf_sys(SYS_CAP_REVOKE,ep,0,0);sfmod_release(&manager,runtime_slot,RES_LEASE);
fail_root:if(directory)sf_ns_close(&ns,1,directory);return r;
}
static int launch_app_root(const char*name,const char*root){return launch_app_root_args(name,root,0);}
static int launch_app(const char*name){return launch_app_root(name,0);}
static void app_reap(uint32_t pid){Child*c=child(pid);if(!c)return;sf_ns_reap(&ns,pid);sf_sys(SYS_CAP_REVOKE,c->reply,0,0);if(c->lease)sfmod_release(&manager,runtime_slot,RES_LEASE);memset(c,0,sizeof(*c));}
static int serve_once(unsigned timeout_ms){
 SfMessage m;long sender=sf_recv(manager_ep,&m,sf_deadline(timeout_ms));if(sender<0)return sender;Child*c=child(sender);if(!c)return SF_INVALID;
 if(m.h.opcode==OP_HELLO)return m.status;
 sf_file_dispatch(&ns,sender,&c->root,&m);return sf_send(c->reply,&m);
}
static int wait_app(uint32_t pid){uint64_t end=sf_ticks()+300;for(;;){long status=sf_sys(SYS_TASK_STATUS,pid,0,0);if(status!=SF_TASK_RUNNING){app_reap(pid);return status;}if(sf_ticks()>end){sf_sys(SYS_KILL,pid,SF_TIMEOUT,0);app_reap(pid);return SF_TIMEOUT;}serve_once(10);sf_yield();}}
static int echo_call(uint32_t,uint32_t);
static int channel_test(int echo_check){
 int a=launch_app_root("readprobe.sfk","/disk/etc"),b=launch_app_root("readprobe.sfk","/disk/etc");int er=echo_check?echo_call(echo_ep,echo_pid):0;int ar=a>0?wait_app(a):a,br=b>0?wait_app(b):b;
 if(!er&&!ar&&!br)sf_log(echo_check?"ECHO CHANNEL TEST PASSED\n":"RPC CHANNEL TEST PASSED\n");else{sf_log(echo_check?"echo-channels: failed e=":"rpc-channels: failed e=");sf_number(er<0?-(int64_t)er:er);sf_log(" a=");sf_number(ar<0?-(int64_t)ar:ar);sf_log(" b=");sf_number(br<0?-(int64_t)br:br);sf_log("\n");}return er?er:ar?ar:br;
}
static int echo_call(uint32_t ep,uint32_t pid){if(manager.modules[echo_slot].state!=MOD_ACTIVE)return SF_DRAINING;EchoPeer*p=echo_peer(pid);if(!p||p->service!=ep)return SF_STALE_HANDLE;if(sf_sys(SYS_TASK_STATUS,pid,0,0)!=SF_TASK_RUNNING)return SF_DEVICE_GONE;SfMessage m;sf_message(&m,OP_ECHO,++requests);memcpy(m.data,"ping",4);m.length=4;int r=sf_send(ep,&m);if(r)return r;long from=sf_recv(p->reply,&m,sf_deadline(1000));return from==pid&&m.h.request_id==requests&&m.length==4&&!memcmp(m.data,"ping",4)?0:SF_INVALID;}
static void echo_capacity_test(void){
 sf_assert(!echo_stop_service(),"echo capacity test stops current instance");int held[64],used=0,r=0;
 while(used<64&&(r=sf_sys(SYS_EP_CREATE,0,0,0))>0)held[used++]=r;
 sf_assert(r==SF_NO_SPACE&&used>0,"echo capacity test exhausts actual endpoint capability slots");
 sf_assert(echo_start_service()==SF_NO_SPACE&&manager.modules[echo_slot].state==MOD_STOPPED,"failed echo service allocation rolls back lifecycle");
 sf_assert(!sf_sys(SYS_CAP_REVOKE,held[--used],0,0),"echo capacity test releases one slot");
 sf_assert(echo_start_service()==SF_NO_SPACE&&manager.modules[echo_slot].state==MOD_STOPPED,"failed echo reply allocation reclaims service endpoint");
 while(used)sf_assert(!sf_sys(SYS_CAP_REVOKE,held[--used],0,0),"echo capacity test reclaims temporary endpoint");
 sf_assert(!echo_start_service()&&!echo_call(echo_ep,echo_pid),"echo restarts after endpoint allocation failures");sf_log("ECHO CAPACITY TEST PASSED\n");
}
static void namespace_setup(void){
 sf_ns_init(&ns,console_write);const char*dirs[]={"/boot","/system","/modules","/bin","/dev","/proc","/sys","/run","/etc","/home","/var","/tmp"};
 for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++)sf_assert(!sf_ns_create(&ns,dirs[i],NODE_DIR,0),"namespace directory");
 sf_ns_create(&ns,"/dev/console",NODE_CONSOLE,0);sf_ns_create(&ns,"/tmp/notes",NODE_FILE,0);sf_ns_create(&ns,"/run/pipe",NODE_PIPE,0);
 int h=sf_ns_open(&ns,1,"/","/tmp/notes",3);sf_ns_write(&ns,1,h,"SpiritFoxOS ramfs\n",18);sf_ns_close(&ns,1,h);
}
static void object_tests(void){
 int a=sf_ns_open(&ns,1,"/","/tmp/notes",3),b=sf_ns_open(&ns,1,"/","/tmp/notes",1);char text[32];
 sf_assert(sf_ns_read(&ns,1,a,text,6)==6&&sf_ns_read(&ns,1,b,text,6)==6&&!memcmp(text,"Spirit",6),"independent open offsets");
 int d=sf_ns_dup(&ns,1,a);sf_ns_read(&ns,1,d,text,3);sf_assert(sf_ns_seek(&ns,1,a,0)==0&&sf_ns_read(&ns,1,d,text,6)==6&&!memcmp(text,"Spirit",6),"duplicate descriptions share offset");
 sf_assert(sf_ns_read(&ns,2,a,text,1)==SF_STALE_HANDLE,"file handles bound to process");sf_ns_close(&ns,1,a);sf_ns_close(&ns,1,d);sf_ns_close(&ns,1,b);
 sf_assert(sf_ns_open(&ns,1,"/home","/tmp/notes",1)==SF_DENIED&&sf_ns_open(&ns,1,"/","/home/../tmp/notes",1)==SF_INVALID,"namespace root and traversal bounds");
 int rd=sf_ns_open(&ns,1,"/","/run/pipe",1),wr=sf_ns_open(&ns,1,"/","/run/pipe",2);
 sf_assert(sf_ns_seek(&ns,1,rd,0)==SF_NOT_SEEKABLE&&sf_ns_read(&ns,1,rd,text,1)==SF_WOULD_BLOCK,"pipe seek and WOULD_BLOCK");
 sf_ns_write(&ns,1,wr,"abc",3);sf_assert((sf_ns_poll(&ns,1,rd)&1)&&sf_ns_read(&ns,1,rd,text,8)==3,"pipe poll and partial read");sf_ns_close(&ns,1,wr);sf_assert(sf_ns_read(&ns,1,rd,text,8)==0,"pipe EOF after final writer");sf_ns_close(&ns,1,rd);
}
static void dependency_generation_tests(void){
 SfManager m={0};int p=sfmod_add(&m,"provider",0,0);sf_assert(p>=0&&!sfmod_start(&m,p),"dependency fixture provider active");
 int c=sfmod_add(&m,"consumer",1u<<p,0);sf_assert(c>=0&&!sfmod_start(&m,c)&&!sfmod_prepare(&m,c)&&!sfmod_stop(&m,c),"dependency fixture lease balanced");
 sf_assert(!sfmod_prepare(&m,p)&&!sfmod_stop(&m,p)&&!sfmod_unload(&m,p),"dependency fixture retires provider registration");
 int q=sfmod_add(&m,"replacement",0,0);sf_assert(q==p&&!sfmod_start(&m,q),"dependency provider slot reused by new generation");
 sf_assert(sfmod_start(&m,c)==SF_DEPENDENCY&&!m.modules[q].resources[RES_LEASE]&&m.modules[c].state==MOD_STOPPED,"stale dependency cannot bind replacement provider");
 sf_log("DEPENDENCY GENERATION TESTS PASSED\n");
}
static void lifecycle_tests(void){
 dependency_generation_tests();
 sf_assert(!echo_call(echo_ep,echo_pid),"user-space echo IPC without filesystem or sfexec");
 sfmod_acquire(&manager,echo_slot,RES_OPEN);sfmod_prepare(&manager,echo_slot);
 sf_assert(sfmod_acquire(&manager,echo_slot,RES_OPEN)==SF_DRAINING&&sfmod_stop(&manager,echo_slot)==SF_BUSY,"drain gate refuses new opens and busy unload");sfmod_cancel(&manager,echo_slot);sf_assert(!echo_call(echo_ep,echo_pid),"cancel stop restores existing service");sfmod_release(&manager,echo_slot,RES_OPEN);
 uint32_t deps[]={2,1};char chain[64];sf_assert(sfmod_cycle(deps,2,chain,sizeof(chain))==SF_DEPENDENCY&&!strcmp(chain,"A>B>A"),"dependency cycle reports A>B>A");
 int a=sfmod_add(&manager,"org.spiritfox.dependent",1u<<echo_slot,0);sfmod_start(&manager,a);sfmod_prepare(&manager,echo_slot);sf_assert(sfmod_stop(&manager,echo_slot)==SF_DEPENDENCY,"active dependent prevents provider unload");sfmod_cancel(&manager,echo_slot);sfmod_prepare(&manager,a);sfmod_stop(&manager,a);sfmod_unload(&manager,a);
 for(unsigned r=RES_TIMER;r<=RES_DMA;r++){sfmod_acquire(&manager,echo_slot,r);sfmod_prepare(&manager,echo_slot);sf_assert(sfmod_stop(&manager,echo_slot)==SF_BUSY,"resource ledger prevents premature reclaim");sfmod_cancel(&manager,echo_slot);sfmod_release(&manager,echo_slot,r);}
 uint32_t old_ep=echo_ep,old_pid=echo_pid,old_gen=manager.modules[echo_slot].generation;int old_slot=echo_slot;
 echo_slot=sfmod_add(&manager,"org.spiritfox.echo",0,0);sfmod_start(&manager,echo_slot);echo_ep=sf_sys(SYS_EP_CREATE,0,0,0);echo_pid=spawn_echo("echo.sfmod",echo_ep,RIGHT_RECV);
 sf_assert(echo_pid>0&&manager.modules[echo_slot].generation>old_gen&&!echo_call(old_ep,old_pid)&&!echo_call(echo_ep,echo_pid),"old and new service generations coexist");
 sfmod_prepare(&manager,old_slot);sfmod_stop(&manager,old_slot);sf_sys(SYS_KILL,old_pid,0,0);echo_forget(old_pid);sf_sys(SYS_CAP_REVOKE,old_ep,0,0);sfmod_unload(&manager,old_slot);SfMessage m;sf_message(&m,OP_ECHO,1);sf_assert(sf_send(old_ep,&m)==SF_STALE_HANDLE,"unloaded endpoint cannot alias a new generation");
}
static void runtime_tests(void){
 SfLoadPlan bad={0};bad.size=sizeof(bad);bad.count=1;bad.segments[0]=(SfLoadSegment){0,4096,(uint64_t)staging,7,0};sf_assert(sf_sys(SYS_SPAWN,(uint64_t)&bad,0,0)==SF_INVALID,"process service rejects actual RWX load plan");
 bad.segments[0].flags=5;bad.device_rights=SF_DEVICE_WRITE;sf_assert(sf_sys(SYS_SPAWN,(uint64_t)&bad,0,0)==SF_INVALID,"process service rejects write right without device");bad.grant_device=1;bad.device_rights=2;sf_assert(sf_sys(SYS_SPAWN,(uint64_t)&bad,0,0)==SF_INVALID,"process service rejects unknown device rights");
 bad.grant_device=bad.device_rights=0;bad.arguments.count=SF_ARGUMENT_MAX+1;sf_assert(sf_sys(SYS_SPAWN,(uint64_t)&bad,0,0)==SF_INVALID,"kernel rejects argument count overflow");
 bad.arguments.count=1;bad.arguments.bytes=5;sf_assert(sf_sys(SYS_SPAWN,(uint64_t)&bad,0,0)==SF_INVALID,"kernel rejects argument offset into header");
 memset(&bad.arguments,0,sizeof(bad.arguments));bad.arguments.env_count=SF_ENVIRONMENT_MAX+1;sf_assert(sf_sys(SYS_SPAWN,(uint64_t)&bad,0,0)==SF_INVALID,"kernel rejects environment count overflow");
 bad.arguments.env_count=0;bad.arguments.reserved=1;sf_assert(sf_sys(SYS_SPAWN,(uint64_t)&bad,0,0)==SF_INVALID,"kernel rejects invocation reserved flags");sf_log("ARGUMENT PLAN TESTS PASSED\n");
 memset(&bad.arguments,0,sizeof(bad.arguments));
 const uint64_t invalid_bases[]={SF_ARGS,SF_STACK,SF_DMA,SF_PIE_MIN+4096,UINT64_MAX};for(unsigned i=0;i<sizeof(invalid_bases)/sizeof(*invalid_bases);i++){bad.image_base=invalid_bases[i];sf_assert(sf_sys(SYS_SPAWN,(uint64_t)&bad,0,0)==SF_INVALID,"process service rejects image base outside isolated arena");}
 sf_log("NATIVE BASE TESTS PASSED\n");
 int ep=sf_sys(SYS_EP_CREATE,0,0,0);SfMessage message;sf_message(&message,OP_ECHO,1);message.h.cap_count=1;sf_assert(sf_send(ep,&message)==SF_INVALID,"message cannot forge transferred capabilities");message.h.cap_count=0;
 for(unsigned i=0;i<8;i++)sf_assert(!sf_send(ep,&message),"bounded IPC queue accepts capacity");sf_assert(sf_send(ep,&message)==SF_WOULD_BLOCK,"full IPC queue applies backpressure");for(unsigned i=0;i<8;i++)sf_recv(ep,&message,sf_deadline(100));sf_assert(sf_recv(ep,&message,sf_deadline(20))==SF_TIMEOUT,"blocked receive expires on monotonic deadline");sf_sys(SYS_CAP_REVOKE,ep,0,0);
 int hello=launch_app("hello.sfk");sf_assert(hello>0,"sfexec validates and launches hello.sfk");sfmod_prepare(&manager,runtime_slot);sf_assert(sfmod_stop(&manager,runtime_slot)==SF_DEPENDENCY,"application pins runtime lease");sfmod_cancel(&manager,runtime_slot);
 sf_assert(wait_app(hello)==0,"hello.sfk writes through namespace RPC and exits");
 int pie_a=launch_app("pieprobe.sfk"),pie_b=launch_app("pieprobe.sfk");int pie_ar=pie_a>0?wait_app(pie_a):pie_a,pie_br=pie_b>0?wait_app(pie_b):pie_b;sf_assert(pie_a>0&&pie_b>0&&!pie_ar&&!pie_br,"two relocated images retain private pointers and independent writable data");sf_log("NATIVE PRIVATE IMAGES PASSED\n");
 int probe=launch_app("probe.sfk");sf_assert(probe>0&&wait_app(probe)==0,"forged handles pointers RWX and privilege amplification rejected");
 int reader=launch_app_root("fileprobe.sfk","/tmp");sf_assert(reader>0&&wait_app(reader)==0,"directory capability reads file and rejects escape and write amplification");
 uint64_t exited_at=sf_ticks();int exited=launch_app("exitbusy.sfk");sf_assert(exited>0&&wait_app(exited)==SF_BUSY&&sf_ticks()-exited_at<100,"exit code SF_BUSY is distinct from a live task");
 int fault=launch_app("fault.sfk");sf_assert(fault>0&&wait_app(fault)==-114&&!echo_call(echo_ep,echo_pid),"user kernel-memory write isolated");
 fault=launch_app("illegal.sfk");sf_assert(fault>0&&wait_app(fault)==-106,"illegal instruction isolated");
 fault=launch_app("nx.sfk");sf_assert(fault>0&&wait_app(fault)==-114,"non-executable user stack enforced");
 fault=launch_app("mmio.sfk");sf_assert(fault>0&&wait_app(fault)==-114,"IOMMU control MMIO inaccessible to ordinary user task");
 fault=launch_app("badrsp.sfk");sf_assert(fault>0&&wait_app(fault)==-113&&!echo_call(echo_ep,echo_pid),"noncanonical user stack is isolated before kernel return");
 int spin=launch_app("spinner.sfk");uint64_t before=sf_ticks();while(sf_ticks()<before+5)sf_yield();sf_assert(spin>0&&sf_sys(SYS_TASK_STATUS,spin,0,0)==SF_TASK_RUNNING&&!echo_call(echo_ep,echo_pid),"timer preempts noncooperating user task");sf_sys(SYS_KILL,spin,0,0);app_reap(spin);
 sfmod_prepare(&manager,runtime_slot);sf_assert(!sfmod_stop(&manager,runtime_slot),"runtime releases lease after application exit");
 sf_assert(launch_app("hello.sfk")==SF_STALE_HANDLE,"missing runtime returns diagnostic error");sfmod_start(&manager,runtime_slot);
 sf_sys(SYS_KILL,runtime_pid,SF_DEVICE_GONE,0);sf_assert(launch_app("hello.sfk")==SF_DEVICE_GONE&&!echo_call(echo_ep,echo_pid),"runtime death is diagnosed without harming other services");sfmod_prepare(&manager,runtime_slot);sfmod_stop(&manager,runtime_slot);sf_sys(SYS_CAP_REVOKE,runtime_ep,0,0);sf_sys(SYS_CAP_REVOKE,runtime_reply,0,0);runtime_reply=sf_sys(SYS_EP_CREATE,0,0,0);sf_assert((int)runtime_reply>0,"runtime reply endpoint recreated after failure");sfmod_start(&manager,runtime_slot);runtime_ep=sf_sys(SYS_EP_CREATE,0,0,0);runtime_pid=spawn_package("sf-native.sfmod",runtime_ep,RIGHT_RECV);sf_assert(runtime_pid>0,"runtime restarts after failure");
}
static void run_command(const char*name,const char*root,const SfArguments*arguments){
 int status=0;if(root){int h=sf_ns_open(&ns,1,"/",root,OPEN_READ|OPEN_DIRECTORY);if(h<0)status=h;else sf_ns_close(&ns,1,h);}
 if(!status){int installed=!strcmp(name,"installed")||!strcmp(name,"/installed.sfk");int trial=installed?sf_persist_begin():0;status=trial;
  if(trial>=0){int pid=launch_app_root_args(name,root,arguments);status=pid>0?wait_app(pid):pid;if(trial){int r=sf_persist_finish(!status);if(r)status=r;}}
 }
 if(!status)sf_log("sfexec: exit 0\n");else{sf_log("sfexec: failed code=");sf_number(status<0?-status:status);sf_log("\n");}
}
static void run_line(char*line,int directory){
 char*words[SF_ARGUMENT_MAX+1];int count=sf_arguments_split(line,words,SF_ARGUMENT_MAX+directory),status=0;unsigned start=directory?1:0;SfArguments arguments;
 if(count<0)status=count;else if((unsigned)count<=start||!words[start][0])status=SF_INVALID;
 if(!status){unsigned argc=count-start;if(argc>1)status=sf_arguments_pack(&arguments,(const char*const*)(words+start),argc);
  if(!status){run_command(words[start],directory?words[0]:0,argc>1?&arguments:0);return;}}
 sf_log("sfexec: failed code=");sf_number(-status);sf_log("\n");
}
static int parse_size(const char*text,uint64_t*out){
 if(!*text)return SF_INVALID;uint64_t n=0;for(;*text;text++){if(*text<'0'||*text>'9')return SF_INVALID;unsigned digit=*text-'0';if(n>(UINT64_MAX-digit)/10)return SF_INVALID;n=n*10+digit;}*out=n;return 0;
}
static void run_environment_line(char*line,int directory){
 char*words[SF_ARGUMENT_MAX+SF_ENVIRONMENT_MAX+2];unsigned start=directory?1:0;int count=sf_arguments_split(line,words,SF_ARGUMENT_MAX+SF_ENVIRONMENT_MAX+1+directory),status=0;SfArguments arguments;
 if(count<0)status=count;else{unsigned end=start;while(end<(unsigned)count&&strcmp(words[end],"--"))end++;
  if(end>=(unsigned)count||end+1>=(unsigned)count||!words[end+1][0])status=SF_INVALID;
  else{status=sf_invocation_pack(&arguments,(const char*const*)(words+end+1),count-end-1,(const char*const*)(words+start),end-start);
   if(!status){run_command(words[end+1],directory?words[0]:0,&arguments);return;}}}
 sf_log("sfexec: failed code=");sf_number(-status);sf_log("\n");
}
static void trust_show(void){
 if(sf_trust_validate(&sf_trust_store)){sf_log("trust: INVALID\n");return;}
 sf_log("trust: build-time keys=");sf_number(sf_trust_store.count);sf_log("\n");
 for(unsigned i=0;i<sf_trust_store.count;i++){const SfTrustKey*k=&sf_trust_store.keys[i];uint8_t hash[32];char id[65];sf_sha256(hash,k->public_key,32);
  for(unsigned j=0;j<32;j++){id[j*2]="0123456789abcdef"[hash[j]>>4];id[j*2+1]="0123456789abcdef"[hash[j]&15];}id[64]=0;
  sf_log("key ");sf_log(id);sf_log(k->revoked?" REVOKED":" ACTIVE");if(k->usage&SF_TRUST_MODULE)sf_log(" module");if(k->usage&SF_TRUST_APP)sf_log(" app");sf_log("\n");
 }
}
static void command(char*line){
 if(!strcmp(line,"help")){
  sf_log("help | ls [path] | cat <path> | write <path> <text> | truncate <path> <bytes> | modules | trust | echo\n");
  sf_log("run <app or installed> [args] | runat <read-only-dir> <app> [args]\nrunenv [NAME=VALUE ...] -- <app> [args] | runenvat <dir> [NAME=VALUE ...] -- <app> [args]\n");
  sf_log("install /disk/bin/app.sfk | persist | stop echo | start echo | mount /disk | unmount /disk | stop disk | start disk | selftest | shutdown\n");}
 else if(!strcmp(line,"persist"))sf_persist_status();
 else if(!strcmp(line,"dma-test write"))sf_storage_dma_probe(0);
 else if(!strcmp(line,"dma-test read"))sf_storage_dma_probe(1);
 else if(!strcmp(line,"disk-crash")){if(sf_storage_crash_test())sf_log("disk: crash test refused\n");}
 else if(!memcmp(line,"install ",8)){int r=sf_persist_install(line+8);if(!r)sf_log("install: COMMITTED\n");else if(r==1)sf_log("install: UNCHANGED\n");else{sf_log("install: failed code=");sf_number(-r);sf_log("\n");}}
 else if(!strcmp(line,"trust"))trust_show();
 else if(!strcmp(line,"modules")){sf_log("sf-init ACTIVE pinned; sf-native ");sf_log(manager.modules[runtime_slot].state==MOD_ACTIVE?"ACTIVE":"STOPPED");sf_log("; echo ");sf_log(manager.modules[echo_slot].state==MOD_ACTIVE?"ACTIVE\n":"STOPPED\n");}
 else if(!strcmp(line,"echo")){sf_log(echo_call(echo_ep,echo_pid)==0?"echo: ping\n":"echo: service unavailable\n");}
 else if(!memcmp(line,"run ",4))run_line(line+4,0);
 else if(!memcmp(line,"runat ",6))run_line(line+6,1);
 else if(!memcmp(line,"runenv ",7))run_environment_line(line+7,0);
 else if(!memcmp(line,"runenvat ",9))run_environment_line(line+9,1);
 else if(!memcmp(line,"truncate ",9)){char*path=line+9,*text=path;while(*text&&*text!=' ')text++;uint64_t bytes=0;int r=SF_INVALID;
  if(*text){*text++=0;r=parse_size(text,&bytes);if(!r){int h=sf_ns_open(&ns,1,"/",path,OPEN_WRITE);r=h<0?h:sf_ns_truncate(&ns,1,h,bytes);if(h>=0)sf_ns_close(&ns,1,h);}}
  if(!r)sf_log("truncate: OK\n");else{sf_log("truncate: failed code=");sf_number(-r);sf_log("\n");}}
 else if(!strcmp(line,"unmount /disk")){int r=sf_storage_unmount();sf_log(!r?"disk: UNMOUNTED\n":"disk: unmount refused\n");}
 else if(!strcmp(line,"mount /disk")){int r=sf_storage_mount();if(!r&&sf_storage_writable())r=sf_persist_mount(&ns);sf_log(!r?"disk: MOUNTED\n":"disk: mount refused\n");}
 else if(!strcmp(line,"stop disk")){int r=sf_storage_stop();sf_log(!r?"disk: STOPPED\n":"disk: stop refused\n");}
 else if(!strcmp(line,"start disk")){int r=sf_storage_start();sf_log(!r?"disk: ACTIVE\n":"disk: start refused\n");}
 else if(!strcmp(line,"channel-test")){channel_test(0);}
 else if(!strcmp(line,"echo-channel-test")){channel_test(1);}
 else if(!strcmp(line,"echo-capacity-test")){echo_capacity_test();}
 else if(!strcmp(line,"stop echo")){sf_log(!echo_stop_service()?"echo: STOPPED\n":"echo: BUSY\n");}
 else if(!strcmp(line,"start echo")){int r=echo_start_service();sf_log(!r?"echo: ACTIVE\n":r==SF_INVALID?"echo: already active\n":"echo: spawn failed\n");}
 else if(!strcmp(line,"ls")||!memcmp(line,"ls ",3)){const char*p=line[2]?line+3:"/";int h=sf_ns_open(&ns,1,"/",p,1);char out[256];long n=h<0?h:sf_ns_enumerate(&ns,1,h,out,sizeof(out));if(n>=0)console_write(out,n);else sf_log("ls: error\n");if(h>=0)sf_ns_close(&ns,1,h);}
 else if(!memcmp(line,"cat ",4)){int h=sf_ns_open(&ns,1,"/",line+4,1);char out[256];long n=h<0?h:sf_ns_read(&ns,1,h,out,sizeof(out));if(n>=0)console_write(out,n);else sf_log("cat: error\n");if(h>=0)sf_ns_close(&ns,1,h);}
 else if(!memcmp(line,"write ",6)){char*p=line+6,*text=p;while(*text&&*text!=' ')text++;if(*text){*text++=0;int h=sf_ns_open(&ns,1,"/",p,2);if(h==SF_NOT_FOUND){sf_ns_create(&ns,p,NODE_FILE,0);h=sf_ns_open(&ns,1,"/",p,2);}long n=h<0?h:sf_ns_write(&ns,1,h,text,strlen(text));sf_log(n>=0?"write: OK\n":"write: error\n");if(h>=0)sf_ns_close(&ns,1,h);}else sf_log("write: expected path and text\n");}
 else if(!strcmp(line,"selftest")){object_tests();sf_assert(!echo_call(echo_ep,echo_pid),"shell echo health");sf_log("SELFTEST OK\n");}
 else if(!strcmp(line,"shutdown")){sf_log("SHUTDOWN OK\n");sf_sys(SYS_SHUTDOWN,0,0,0);}
 else if(*line)sf_log("unknown command; type help\n");
}
long sf_main(const SfBootArgs*a,size_t size){
 boot=a;if(size!=sizeof(*a)||a->abi_major!=1||!a->bundle)return SF_INVALID;
 sf_log("[INIT] sf-init.sfmod in ring 3; services live outside Core 0\n");manager_ep=sf_sys(SYS_EP_CREATE,0,0,0);
 int init=sfmod_add(&manager,"org.spiritfox.init",0,1);sfmod_start(&manager,init);runtime_slot=sfmod_add(&manager,"org.spiritfox.native",0,0);sfmod_start(&manager,runtime_slot);
 echo_slot=sfmod_add(&manager,"org.spiritfox.echo",0,0);sfmod_start(&manager,echo_slot);echo_ep=sf_sys(SYS_EP_CREATE,0,0,0);echo_pid=spawn_echo("echo.sfmod",echo_ep,RIGHT_RECV);sf_assert((int)echo_pid>0,"verified echo.sfmod staged in private address space");
 lifecycle_tests();namespace_setup();object_tests();runtime_reply=sf_sys(SYS_EP_CREATE,0,0,0);sf_assert((int)runtime_reply>0,"runtime replies have an isolated endpoint");runtime_ep=sf_sys(SYS_EP_CREATE,0,0,0);runtime_pid=spawn_package("sf-native.sfmod",runtime_ep,RIGHT_RECV);sf_assert(runtime_pid>0,"native runtime module active");runtime_tests();
 sf_storage_init(&manager,&ns,spawn_storage_package);int storage=sf_storage_start();if(!storage)storage=sf_storage_mount();
 if(storage){sf_log("STORAGE UNAVAILABLE code=");sf_number(-storage);sf_log("\n");}
 else{sf_storage_tests();if(sf_storage_writable()){int r=sf_persist_mount(&ns);if(r){sf_log("PERSIST UNAVAILABLE code=");sf_number(-r);sf_log("\n");}}int ep=sf_sys(SYS_EP_CREATE,0,0,0);int disk_module=spawn_echo("/disk/modules/disk-echo.sfmod",ep,RIGHT_RECV);
  if(disk_module>0){int slot=sfmod_add(&manager,"org.spiritfox.disk-echo",0,0);sf_assert(slot>=0&&!sfmod_start(&manager,slot)&&!echo_call(ep,disk_module),"signed sfmod loaded from disk without bundle fallback");sfmod_prepare(&manager,slot);sfmod_stop(&manager,slot);sf_sys(SYS_KILL,disk_module,0,0);echo_forget(disk_module);sfmod_unload(&manager,slot);}else{sf_log("DISK PACKAGE REJECTED module code=");sf_number(-disk_module);sf_log("\n");}sf_sys(SYS_CAP_REVOKE,ep,0,0);
  int app=launch_app("/disk/bin/diskhello.sfk");if(app>0)sf_assert(wait_app(app)==0,"signed disk application runs through native runtime");else{sf_log("DISK PACKAGE REJECTED app code=");sf_number(-app);sf_log("\n");}}
 sf_log("BOOT TESTS PASSED\nSpiritFoxOS ready. Type help.\nfox> ");char line[192]={0};unsigned used=0;
 for(;;){long c=sf_sys(SYS_CONSOLE_READ,0,0,0);if(c>=0){if(c=='\r'||c=='\n'){sf_log("\n");line[used]=0;command(line);used=0;memset(line,0,sizeof(line));sf_log("fox> ");}else if(c==8||c==127){if(used){used--;sf_log("\b \b");}}else if(c>=32&&c<127&&used+1<sizeof(line)){line[used++]=c;char s[2]={(char)c,0};sf_log(s);}}else if(c==SF_EVENT_OVERFLOW){used=0;memset(line,0,sizeof(line));sf_log("\nconsole input overflow; line discarded\nfox> ");}else{serve_once(10);sf_yield();}}
}
