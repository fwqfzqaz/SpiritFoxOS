#include "libsf.h"
#include "package.h"
#include "manager.h"
#include "namespace.h"
#include "storage.h"
#include "persistence.h"
#include "trust.h"
static const SfBootArgs*boot;
static uint32_t manager_ep;static SfManager manager;static SfNamespace ns;
static int runtime_slot,echo_slot;static uint32_t runtime_pid,runtime_ep,echo_pid,echo_ep;
static uint8_t staging[1024*1024] __attribute__((aligned(4096)));
static uint8_t package_snapshot[SF_READFS_FILE_MAX];
typedef struct {uint32_t pid,reply;int lease;} Child;
static Child children[16];static uint64_t requests;
static long console_write(const void*p,size_t n){return sf_sys(SYS_LOG,(uint64_t)p,n,0);}
static int package_get(const char*name,SfPackage*p){const uint8_t*d;uint64_t n;if(name[0]=='/'){long r=!strcmp(name,"/installed.sfk")?sf_persist_package(package_snapshot,sizeof(package_snapshot)):sf_storage_load(name,package_snapshot,sizeof(package_snapshot));return r<0?r:sf_package_open(p,package_snapshot,r,sf_trust_key);}int r=sf_bundle_find((void*)boot->bundle,boot->bundle_bytes,name,&d,&n);return r?r:sf_package_open(p,d,n,sf_trust_key);}
static int native_upload(const SfPackage*p){for(uint64_t off=0;off<p->bytes;){SfMessage m;sf_message(&m,OP_NATIVE_STAGE,++requests);m.handle=p->bytes;m.offset=off;m.length=p->bytes-off;if(m.length>sizeof(m.data))m.length=sizeof(m.data);uint32_t sent=m.length;memcpy(m.data,p->data+off,sent);int rc=sf_send(runtime_ep,&m);if(rc)return rc;long from=sf_recv(manager_ep,&m,sf_deadline(1000));if(from!=(long)runtime_pid||m.h.request_id!=requests)return from<0?from:SF_STALE_HANDLE;if(m.status)return m.status;off+=sent;if(m.offset!=off)return SF_BAD_FORMAT;}return 0;}
static int spawn_package_device(const char*name,uint32_t service,uint32_t rights,uint32_t device,uint32_t device_rights){
 SfPackage p;int r=package_get(name,&p);if(r)return r;
 if(p.kind==2){char id[96];if(sf_manifest_text(&p,"runtime_id",id,sizeof(id))||strcmp(id,"org.spiritfox.native")||manager.modules[runtime_slot].state!=MOD_ACTIVE)return SF_DEPENDENCY;
  if(sf_sys(SYS_TASK_STATUS,runtime_pid,0,0)!=SF_TASK_RUNNING)return SF_DEVICE_GONE;
  int external=name[0]=='/';if(external){int status=native_upload(&p);if(status)return status;}
  SfMessage request;sf_message(&request,OP_NATIVE_PREPARE,++requests);request.length=strlen(name)+1;if(request.length>sizeof(request.data))return SF_INVALID;request.mode=external;memcpy(request.data,name,request.length);int rc=sf_send(runtime_ep,&request);if(rc)return rc;long from=sf_recv(manager_ep,&request,sf_deadline(1000));if(from!=(long)runtime_pid||request.h.request_id!=requests)return from<0?from:SF_INVALID;if(request.status)return request.status;
  if(request.length!=sizeof(SfNativePlan))return SF_BAD_FORMAT;SfNativePlan native;memcpy(&native,request.data,sizeof(native));if(native.entry!=p.entry||native.reserved||native.count>5)return SF_BAD_FORMAT;unsigned j=0;for(unsigned i=0;i<p.count;i++){SfSection*s=&p.sections[i];if(s->type>3)continue;if(j>=native.count)return SF_BAD_FORMAT;SfNativeSegment*t=&native.segments[j++];if(t->rva!=s->rva||t->memory!=s->memory||t->offset!=s->offset||t->bytes!=s->bytes||t->flags!=s->flags||t->reserved)return SF_BAD_FORMAT;}if(j!=native.count)return SF_BAD_FORMAT;
 }
 SfLoadPlan plan={0};plan.size=sizeof(plan);plan.entry=p.entry;plan.grant_manager=manager_ep;plan.grant_service=service;plan.service_rights=rights;plan.grant_bundle=!strcmp(name,"sf-native.sfmod");
 plan.grant_device=device;plan.device_rights=device_rights;
 size_t used=0;for(unsigned i=0;i<p.count;i++){SfSection*s=&p.sections[i];if(s->type>3)continue;size_t mem=(s->memory+4095)&~4095ull;if(mem>sizeof(staging)-used||plan.count==8)return SF_NO_SPACE;memset(staging+used,0,mem);memcpy(staging+used,p.data+s->offset,s->bytes);plan.segments[plan.count++]=(SfLoadSegment){s->rva,s->memory,(uint64_t)(staging+used),s->flags,0};used+=mem;}
 return sf_sys(SYS_SPAWN,(uint64_t)&plan,0,0);
}
static int spawn_package(const char*name,uint32_t service,uint32_t rights){return spawn_package_device(name,service,rights,0,0);}
static Child*child(uint32_t pid){for(unsigned i=0;i<16;i++)if(children[i].pid==pid)return&children[i];return 0;}
static int launch_app(const char*name){
 if(!strcmp(name,"installed"))name="/installed.sfk";
 int slot=-1;for(unsigned i=0;i<16;i++)if(!children[i].pid){slot=i;break;}if(slot<0)return SF_NO_SPACE;
 int r=sfmod_acquire(&manager,runtime_slot,RES_LEASE);if(r)return r;int ep=sf_sys(SYS_EP_CREATE,0,0,0);if(ep<0){sfmod_release(&manager,runtime_slot,RES_LEASE);return ep;}
 int pid=spawn_package(name,ep,RIGHT_RECV);if(pid<0){sf_sys(SYS_CAP_REVOKE,ep,0,0);sfmod_release(&manager,runtime_slot,RES_LEASE);return pid;}children[slot]=(Child){pid,ep,1};return pid;
}
static void app_reap(uint32_t pid){Child*c=child(pid);if(!c)return;sf_ns_reap(&ns,pid);sf_sys(SYS_CAP_REVOKE,c->reply,0,0);if(c->lease)sfmod_release(&manager,runtime_slot,RES_LEASE);memset(c,0,sizeof(*c));}
static int serve_once(unsigned timeout_ms){
 SfMessage m;long sender=sf_recv(manager_ep,&m,sf_deadline(timeout_ms));if(sender<0)return sender;Child*c=child(sender);if(!c)return SF_INVALID;
 if(m.h.opcode==OP_HELLO)return m.status;
 int status=SF_INVALID;
 switch(m.h.opcode){
 case OP_OPEN:
  if(!m.length||m.length>SF_MSG_BYTES||m.data[m.length-1]){status=SF_INVALID;break;}
  /* Initial application policy delegates stdout only; no filesystem root. */
  if(strcmp((char*)m.data,"/dev/console")||m.mode!=OPEN_WRITE){status=SF_DENIED;break;}
  status=sf_ns_open(&ns,sender,"/",(char*)m.data,m.mode);if(status>=0){m.handle=status;status=0;}break;
 case OP_WRITE:status=sf_ns_write(&ns,sender,m.handle,m.data,m.length);break;
 case OP_READ:status=sf_ns_read(&ns,sender,m.handle,m.data,m.length);if(status>=0)m.length=status;break;
 case OP_CLOSE:status=sf_ns_close(&ns,sender,m.handle);break;
 case OP_SEEK:status=sf_ns_seek(&ns,sender,m.handle,m.offset);break;
 case OP_POLL:status=sf_ns_poll(&ns,sender,m.handle);break;
 }
 m.status=status;return sf_send(c->reply,&m);
}
static int wait_app(uint32_t pid){uint64_t end=sf_ticks()+300;for(;;){long status=sf_sys(SYS_TASK_STATUS,pid,0,0);if(status!=SF_TASK_RUNNING){app_reap(pid);return status;}if(sf_ticks()>end){sf_sys(SYS_KILL,pid,SF_TIMEOUT,0);app_reap(pid);return SF_TIMEOUT;}serve_once(10);sf_yield();}}
static int echo_call(uint32_t ep,uint32_t pid){if(manager.modules[echo_slot].state!=MOD_ACTIVE)return SF_DRAINING;SfMessage m;sf_message(&m,OP_ECHO,++requests);memcpy(m.data,"ping",4);m.length=4;int r=sf_send(ep,&m);if(r)return r;long from=sf_recv(manager_ep,&m,sf_deadline(1000));return from==pid&&m.h.request_id==requests&&m.length==4&&!memcmp(m.data,"ping",4)?0:SF_INVALID;}
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
static void lifecycle_tests(void){
 sf_assert(!echo_call(echo_ep,echo_pid),"user-space echo IPC without filesystem or sfexec");
 sfmod_acquire(&manager,echo_slot,RES_OPEN);sfmod_prepare(&manager,echo_slot);
 sf_assert(sfmod_acquire(&manager,echo_slot,RES_OPEN)==SF_DRAINING&&sfmod_stop(&manager,echo_slot)==SF_BUSY,"drain gate refuses new opens and busy unload");sfmod_cancel(&manager,echo_slot);sf_assert(!echo_call(echo_ep,echo_pid),"cancel stop restores existing service");sfmod_release(&manager,echo_slot,RES_OPEN);
 uint32_t deps[]={2,1};char chain[64];sf_assert(sfmod_cycle(deps,2,chain,sizeof(chain))==SF_DEPENDENCY&&!strcmp(chain,"A>B>A"),"dependency cycle reports A>B>A");
 int a=sfmod_add(&manager,"org.spiritfox.dependent",1u<<echo_slot,0);sfmod_start(&manager,a);sfmod_prepare(&manager,echo_slot);sf_assert(sfmod_stop(&manager,echo_slot)==SF_DEPENDENCY,"active dependent prevents provider unload");sfmod_cancel(&manager,echo_slot);sfmod_prepare(&manager,a);sfmod_stop(&manager,a);sfmod_unload(&manager,a);
 for(unsigned r=RES_TIMER;r<=RES_DMA;r++){sfmod_acquire(&manager,echo_slot,r);sfmod_prepare(&manager,echo_slot);sf_assert(sfmod_stop(&manager,echo_slot)==SF_BUSY,"resource ledger prevents premature reclaim");sfmod_cancel(&manager,echo_slot);sfmod_release(&manager,echo_slot,r);}
 uint32_t old_ep=echo_ep,old_pid=echo_pid,old_gen=manager.modules[echo_slot].generation;int old_slot=echo_slot;
 echo_slot=sfmod_add(&manager,"org.spiritfox.echo",0,0);sfmod_start(&manager,echo_slot);echo_ep=sf_sys(SYS_EP_CREATE,0,0,0);echo_pid=spawn_package("echo.sfmod",echo_ep,RIGHT_RECV);
 sf_assert(echo_pid>0&&manager.modules[echo_slot].generation>old_gen&&!echo_call(old_ep,old_pid)&&!echo_call(echo_ep,echo_pid),"old and new service generations coexist");
 sfmod_prepare(&manager,old_slot);sfmod_stop(&manager,old_slot);sf_sys(SYS_KILL,old_pid,0,0);sf_sys(SYS_CAP_REVOKE,old_ep,0,0);sfmod_unload(&manager,old_slot);SfMessage m;sf_message(&m,OP_ECHO,1);sf_assert(sf_send(old_ep,&m)==SF_STALE_HANDLE,"unloaded endpoint cannot alias a new generation");
}
static void runtime_tests(void){
 SfLoadPlan bad={0};bad.size=sizeof(bad);bad.count=1;bad.segments[0]=(SfLoadSegment){0,4096,(uint64_t)staging,7,0};sf_assert(sf_sys(SYS_SPAWN,(uint64_t)&bad,0,0)==SF_INVALID,"process service rejects actual RWX load plan");
 bad.segments[0].flags=5;bad.device_rights=SF_DEVICE_WRITE;sf_assert(sf_sys(SYS_SPAWN,(uint64_t)&bad,0,0)==SF_INVALID,"process service rejects write right without device");bad.grant_device=1;bad.device_rights=2;sf_assert(sf_sys(SYS_SPAWN,(uint64_t)&bad,0,0)==SF_INVALID,"process service rejects unknown device rights");
 int ep=sf_sys(SYS_EP_CREATE,0,0,0);SfMessage message;sf_message(&message,OP_ECHO,1);message.h.cap_count=1;sf_assert(sf_send(ep,&message)==SF_INVALID,"message cannot forge transferred capabilities");message.h.cap_count=0;
 for(unsigned i=0;i<8;i++)sf_assert(!sf_send(ep,&message),"bounded IPC queue accepts capacity");sf_assert(sf_send(ep,&message)==SF_WOULD_BLOCK,"full IPC queue applies backpressure");for(unsigned i=0;i<8;i++)sf_recv(ep,&message,sf_deadline(100));sf_assert(sf_recv(ep,&message,sf_deadline(20))==SF_TIMEOUT,"blocked receive expires on monotonic deadline");sf_sys(SYS_CAP_REVOKE,ep,0,0);
 int hello=launch_app("hello.sfk");sf_assert(hello>0,"sfexec validates and launches hello.sfk");sfmod_prepare(&manager,runtime_slot);sf_assert(sfmod_stop(&manager,runtime_slot)==SF_DEPENDENCY,"application pins runtime lease");sfmod_cancel(&manager,runtime_slot);
 sf_assert(wait_app(hello)==0,"hello.sfk writes through namespace RPC and exits");
 int probe=launch_app("probe.sfk");sf_assert(probe>0&&wait_app(probe)==0,"forged handles pointers RWX and privilege amplification rejected");
 uint64_t exited_at=sf_ticks();int exited=launch_app("exitbusy.sfk");sf_assert(exited>0&&wait_app(exited)==SF_BUSY&&sf_ticks()-exited_at<100,"exit code SF_BUSY is distinct from a live task");
 int fault=launch_app("fault.sfk");sf_assert(fault>0&&wait_app(fault)==-114&&!echo_call(echo_ep,echo_pid),"user kernel-memory write isolated");
 fault=launch_app("illegal.sfk");sf_assert(fault>0&&wait_app(fault)==-106,"illegal instruction isolated");
 fault=launch_app("nx.sfk");sf_assert(fault>0&&wait_app(fault)==-114,"non-executable user stack enforced");
 fault=launch_app("mmio.sfk");sf_assert(fault>0&&wait_app(fault)==-114,"IOMMU control MMIO inaccessible to ordinary user task");
 fault=launch_app("badrsp.sfk");sf_assert(fault>0&&wait_app(fault)==-113&&!echo_call(echo_ep,echo_pid),"noncanonical user stack is isolated before kernel return");
 int spin=launch_app("spinner.sfk");uint64_t before=sf_ticks();while(sf_ticks()<before+5)sf_yield();sf_assert(spin>0&&sf_sys(SYS_TASK_STATUS,spin,0,0)==SF_TASK_RUNNING&&!echo_call(echo_ep,echo_pid),"timer preempts noncooperating user task");sf_sys(SYS_KILL,spin,0,0);app_reap(spin);
 sfmod_prepare(&manager,runtime_slot);sf_assert(!sfmod_stop(&manager,runtime_slot),"runtime releases lease after application exit");
 sf_assert(launch_app("hello.sfk")==SF_STALE_HANDLE,"missing runtime returns diagnostic error");sfmod_start(&manager,runtime_slot);
 sf_sys(SYS_KILL,runtime_pid,SF_DEVICE_GONE,0);sf_assert(launch_app("hello.sfk")==SF_DEVICE_GONE&&!echo_call(echo_ep,echo_pid),"runtime death is diagnosed without harming other services");sfmod_prepare(&manager,runtime_slot);sfmod_stop(&manager,runtime_slot);sf_sys(SYS_CAP_REVOKE,runtime_ep,0,0);sfmod_start(&manager,runtime_slot);runtime_ep=sf_sys(SYS_EP_CREATE,0,0,0);runtime_pid=spawn_package("sf-native.sfmod",runtime_ep,RIGHT_RECV);sf_assert(runtime_pid>0,"runtime restarts after failure");
}
static void command(char*line){
 if(!strcmp(line,"help")){sf_log("help | ls [path] | cat <path> | write <path> <text> | modules | echo | run <app.sfk or installed> | install /disk/bin/app.sfk | persist | stop echo | start echo | mount /disk | unmount /disk | stop disk | start disk | selftest | shutdown\n");}
 else if(!strcmp(line,"persist"))sf_persist_status();
 else if(!strcmp(line,"dma-test write"))sf_storage_dma_probe(0);
 else if(!strcmp(line,"dma-test read"))sf_storage_dma_probe(1);
 else if(!strcmp(line,"disk-crash")){if(sf_storage_crash_test())sf_log("disk: crash test refused\n");}
 else if(!memcmp(line,"install ",8)){int r=sf_persist_install(line+8);if(!r)sf_log("install: COMMITTED\n");else if(r==1)sf_log("install: UNCHANGED\n");else{sf_log("install: failed code=");sf_number(-r);sf_log("\n");}}
 else if(!strcmp(line,"modules")){sf_log("sf-init ACTIVE pinned; sf-native ");sf_log(manager.modules[runtime_slot].state==MOD_ACTIVE?"ACTIVE":"STOPPED");sf_log("; echo ");sf_log(manager.modules[echo_slot].state==MOD_ACTIVE?"ACTIVE\n":"STOPPED\n");}
 else if(!strcmp(line,"echo")){sf_log(echo_call(echo_ep,echo_pid)==0?"echo: ping\n":"echo: service unavailable\n");}
 else if(!memcmp(line,"run ",4)){
  int installed=!strcmp(line+4,"installed")||!strcmp(line+4,"/installed.sfk");int trial=installed?sf_persist_begin():0,status=trial;
  if(trial>=0){int pid=launch_app(line+4);status=pid>0?wait_app(pid):pid;if(trial){int r=sf_persist_finish(!status);if(r)status=r;}}
  if(!status)sf_log("sfexec: exit 0\n");else{sf_log("sfexec: failed code=");sf_number(status<0?-status:status);sf_log("\n");}
 }
 else if(!strcmp(line,"unmount /disk")){int r=sf_storage_unmount();sf_log(!r?"disk: UNMOUNTED\n":"disk: unmount refused\n");}
 else if(!strcmp(line,"mount /disk")){int r=sf_storage_mount();if(!r&&sf_storage_writable())r=sf_persist_mount(&ns);sf_log(!r?"disk: MOUNTED\n":"disk: mount refused\n");}
 else if(!strcmp(line,"stop disk")){int r=sf_storage_stop();sf_log(!r?"disk: STOPPED\n":"disk: stop refused\n");}
 else if(!strcmp(line,"start disk")){int r=sf_storage_start();sf_log(!r?"disk: ACTIVE\n":"disk: start refused\n");}
 else if(!strcmp(line,"stop echo")){int r=sfmod_prepare(&manager,echo_slot);if(!r)r=sfmod_stop(&manager,echo_slot);if(!r){sf_sys(SYS_KILL,echo_pid,0,0);sf_sys(SYS_CAP_REVOKE,echo_ep,0,0);sf_log("echo: STOPPED\n");}else{sfmod_cancel(&manager,echo_slot);sf_log("echo: BUSY\n");}}
 else if(!strcmp(line,"start echo")){int r=sfmod_start(&manager,echo_slot);if(!r){echo_ep=sf_sys(SYS_EP_CREATE,0,0,0);int pid=spawn_package("echo.sfmod",echo_ep,RIGHT_RECV);if(pid>0){echo_pid=pid;sf_log("echo: ACTIVE\n");}else sf_log("echo: spawn failed\n");}else sf_log("echo: already active\n");}
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
 echo_slot=sfmod_add(&manager,"org.spiritfox.echo",0,0);sfmod_start(&manager,echo_slot);echo_ep=sf_sys(SYS_EP_CREATE,0,0,0);echo_pid=spawn_package("echo.sfmod",echo_ep,RIGHT_RECV);sf_assert((int)echo_pid>0,"verified echo.sfmod staged in private address space");
 lifecycle_tests();namespace_setup();object_tests();runtime_ep=sf_sys(SYS_EP_CREATE,0,0,0);runtime_pid=spawn_package("sf-native.sfmod",runtime_ep,RIGHT_RECV);sf_assert(runtime_pid>0,"native runtime module active");runtime_tests();
 sf_storage_init(&manager,&ns,manager_ep,spawn_package_device);int storage=sf_storage_start();if(!storage)storage=sf_storage_mount();
 if(storage){sf_log("STORAGE UNAVAILABLE code=");sf_number(-storage);sf_log("\n");}
 else{sf_storage_tests();if(sf_storage_writable()){int r=sf_persist_mount(&ns);if(r){sf_log("PERSIST UNAVAILABLE code=");sf_number(-r);sf_log("\n");}}int ep=sf_sys(SYS_EP_CREATE,0,0,0);int disk_module=spawn_package("/disk/modules/disk-echo.sfmod",ep,RIGHT_RECV);
  if(disk_module>0){int slot=sfmod_add(&manager,"org.spiritfox.disk-echo",0,0);sf_assert(slot>=0&&!sfmod_start(&manager,slot)&&!echo_call(ep,disk_module),"signed sfmod loaded from disk without bundle fallback");sfmod_prepare(&manager,slot);sfmod_stop(&manager,slot);sf_sys(SYS_KILL,disk_module,0,0);sfmod_unload(&manager,slot);}else{sf_log("DISK PACKAGE REJECTED module code=");sf_number(-disk_module);sf_log("\n");}sf_sys(SYS_CAP_REVOKE,ep,0,0);
  int app=launch_app("/disk/bin/diskhello.sfk");if(app>0)sf_assert(wait_app(app)==0,"signed disk application runs through native runtime");else{sf_log("DISK PACKAGE REJECTED app code=");sf_number(-app);sf_log("\n");}}
 sf_log("BOOT TESTS PASSED\nSpiritFoxOS ready. Type help.\nfox> ");char line[192]={0};unsigned used=0;
 for(;;){long c=sf_sys(SYS_CONSOLE_READ,0,0,0);if(c>=0){if(c=='\r'||c=='\n'){sf_log("\n");line[used]=0;command(line);used=0;memset(line,0,sizeof(line));sf_log("fox> ");}else if(c==8||c==127){if(used){used--;sf_log("\b \b");}}else if(c>=32&&c<127&&used+1<sizeof(line)){line[used++]=c;char s[2]={(char)c,0};sf_log(s);}}else if(c==SF_EVENT_OVERFLOW){used=0;memset(line,0,sizeof(line));sf_log("\nconsole input overflow; line discarded\nfox> ");}else{serve_once(10);sf_yield();}}
}
