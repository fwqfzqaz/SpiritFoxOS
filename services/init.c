#include "libsf.h"
#include "package.h"
#include "manager.h"
#include "supervisor.h"
#include "namespace.h"
#include "storage.h"
#include "persistence.h"
#include "trust.h"
#include "arguments.h"
#include "native_image.h"
#include "file_rpc.h"
#include "lifecycle_rpc.h"
#include "net.h"
#include "accounts.h"
#include "session.h"
static const SfBootArgs*boot;
static uint32_t manager_ep,fs_ep,manager_reply;static SfManager manager;static SfNamespace ns;
static int runtime_slot,echo_slot;static uint32_t runtime_pid,runtime_ep,runtime_reply,echo_pid,echo_ep;
/* 单应用映像的实际上限即此暂存区容量（1280 KiB）：sf-persist.sfmod 的
 * 事务卷常驻数据与 sf-fs.sfmod 的 SFRFS2 校验缓冲都经同一装载路径进入
 * 各自私有地址空间，整体仍受 2 MiB 可写段预算约束。 */
static uint8_t staging[1280*1024] __attribute__((aligned(4096)));
static unsigned pie_sequence;
typedef struct {uint32_t pid,reply,root;int lease;uint32_t uid,gid;} Child;
static Child children[16];static uint64_t requests;
/* Identity is user-space policy: the kernel only authenticates the sender pid,
 * so sf-init maps that pid to the session identity that launched it. */
static SfAccountStore accounts;static SfSessionTable sessions;
static int password_pending;static char password_user[SF_ACCOUNT_NAME_MAX+1];static char password_buffer[SF_ACCOUNT_PASSWORD_MAX+1];static unsigned password_used;
typedef struct {uint32_t pid,service,reply;} EchoPeer;
static int net_slot=-1;static uint32_t net_pid,net_ep,net_reply;static int net_ready,net_dhcp;static SfNetInfo net_info;
static EchoPeer echo_peers[16];static SfSupervisor echo_watch;static uint64_t echo_health_at;static SfSupervisor native_watch;static uint64_t native_health_at;static int native_draining;
static int echo_request(uint32_t,uint32_t,unsigned);
static uint64_t echo_after(uint64_t now,uint64_t delay){return delay>UINT64_MAX-now?UINT64_MAX:now+delay;}
static long console_write(const void*p,size_t n){return sf_sys(SYS_LOG,(uint64_t)p,n,0);}
#define SF_POSIX_LIBRARY_FILE "sf-libsf.sfmod"
#define SF_POSIX_LIBRARY_ID "org.spiritfox.posix"
static int package_get(const char*name,SfPackage*p){const uint8_t*d;uint64_t n;if(name[0]=='/'){uint8_t*snapshot=sf_storage_scratch();long r=!strcmp(name,"/installed.sfk")?sf_persist_package(snapshot,SF_STORAGE_SNAPSHOT_BYTES):sf_storage_load(name,snapshot,SF_STORAGE_SNAPSHOT_BYTES);return r<0?r:sf_trust_open(&sf_trust,p,snapshot,r);}int r=sf_bundle_find((void*)boot->bundle,boot->bundle_bytes,name,&d,&n);return r?r:sf_trust_open(&sf_trust,p,d,n);}
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
 SfLoadPlan plan={0};plan.size=sizeof(plan);plan.entry=p.entry;plan.grant_manager=reply?reply:native_module?runtime_reply:fs_ep;plan.grant_service=service;plan.service_rights=rights;plan.grant_bundle=native_module;
 plan.grant_device=device;plan.device_rights=device_rights;
 if(arguments)plan.arguments=*arguments;
 SfPackage library;int linked=0;
 if(p.kind==2&&p.import_count){
  char id[96];if(sf_native_library(&p,id,sizeof(id))||strcmp(id,SF_POSIX_LIBRARY_ID))return SF_DEPENDENCY;
  r=package_get(SF_POSIX_LIBRARY_FILE,&library);if(r)return r;
  if(library.kind!=1||library.native_profile!=SF_NATIVE_RELATIVE||library.native_role!=SF_NATIVE_ROLE_LIBRARY||!library.export_count||sf_manifest_text(&library,"module_id",id,sizeof(id))||strcmp(id,SF_POSIX_LIBRARY_ID))return SF_DEPENDENCY;
  linked=1;
 }
 r=linked?sf_native_link(&p,&library,image_base,staging,sizeof(staging),&plan):sf_native_image(&p,image_base,staging,sizeof(staging),&plan);if(r)return r;
 return sf_sys(SYS_SPAWN,(uint64_t)&plan,0,0);
}
static int spawn_package_device(const char*name,uint32_t service,uint32_t rights,uint32_t device,uint32_t device_rights){return spawn_package_policy(name,service,rights,device,device_rights,0,0,0,1);}
static int spawn_storage_package(const char*name,uint32_t service,uint32_t rights,uint32_t device,uint32_t device_rights,uint32_t reply){return spawn_package_policy(name,service,rights,device,device_rights,0,0,reply,1);}
static int spawn_package(const char*name,uint32_t service,uint32_t rights){return spawn_package_device(name,service,rights,0,0);}
static int net_call(SfMessage*m){
 if(!net_pid||sf_sys(SYS_TASK_STATUS,net_pid,0,0)!=SF_TASK_RUNNING)return SF_DEVICE_GONE;
 uint64_t id=++requests;m->h.request_id=id;int r=sf_send(net_ep,m);if(r)return r;
 long from=sf_recv(net_reply,m,sf_deadline(2000));if(from<0){sf_sys(SYS_KILL,net_pid,SF_DEVICE_GONE,0);return(int)from;}
 if(from!=(long)net_pid||m->h.request_id!=id)return SF_STALE_HANDLE;return(int)m->status;
}
/* Kernel grants up to two devices concurrently, each with its own DMA arena and
 * IOMMU window, so a disk boot and a network boot no longer compete. */
static int net_start(void){
 if(net_pid||net_ep||net_reply)return SF_BUSY;
 uint32_t device=0;for(unsigned d=0;d<32&&!device;d++)for(unsigned f=0;f<8;f++){uint32_t bdf=d*8+f;long id=sf_sys(SYS_PCI_READ,bdf,0,0);if(id==0x10001af4u){device=bdf+1;break;}}
 if(!device)return SF_NOT_FOUND;
 if(net_slot<0){net_slot=sfmod_add(&manager,"org.spiritfox.virtio.net",0,0);if(net_slot<0)return net_slot;}
 int r=sfmod_start(&manager,net_slot);if(r)return r;
 net_reply=sf_sys(SYS_EP_CREATE,0,0,0);if((int)net_reply<0){r=(int)net_reply;net_reply=0;goto fail;}
 net_ep=sf_sys(SYS_EP_CREATE,0,0,0);if((int)net_ep<0){r=(int)net_ep;net_ep=0;goto fail;}
 int pid=spawn_storage_package("sf-virtio-net.sfmod",net_ep,RIGHT_RECV,device,0,net_reply);if(pid<0){r=pid;goto fail;}
 net_pid=pid;SfMessage m;sf_message(&m,OP_NET_INFO,0);r=net_call(&m);if(r)goto fail;
 if(m.length!=sizeof(SfNetInfo)){r=SF_BAD_FORMAT;goto fail;}
 SfNetInfo info;memcpy(&info,m.data,sizeof(info));
 if(info.reserved||!info.link||!info.ip||!info.gateway||!info.netmask){r=SF_BAD_FORMAT;goto fail;}
 net_info=info;sf_message(&m,OP_NET_DHCP,0);r=net_call(&m);
 if(r||m.length!=sizeof(SfNetInfo)){r=SF_BAD_FORMAT;goto fail;}
 memcpy(&info,m.data,sizeof(info));net_dhcp=(int)m.mode;sfmod_acquire(&manager,net_slot,RES_DMA);net_ready=1;return 0;
fail:
 if(net_pid)sf_sys(SYS_KILL,net_pid,SF_DEVICE_GONE,0);net_pid=0;
 if(net_ep)sf_sys(SYS_CAP_REVOKE,net_ep,0,0);net_ep=0;
 if(net_reply)sf_sys(SYS_CAP_REVOKE,net_reply,0,0);net_reply=0;
 sfmod_prepare(&manager,net_slot);sfmod_stop(&manager,net_slot);net_ready=0;return r;
}
static void net_status(void){
 if(!net_ready){sf_log("net: DOWN\n");return;}
 SfMessage m;sf_message(&m,OP_NET_INFO,0);int r=net_call(&m);
 if(r||m.length!=sizeof(SfNetInfo)){sf_log("net: status failed code=");sf_number(r<0?-r:r);sf_log("\n");return;}
 memcpy(&net_info,m.data,sizeof(net_info));
 char mac[18];for(unsigned i=0;i<6;i++){mac[i*3]="0123456789abcdef"[net_info.mac[i]>>4];mac[i*3+1]="0123456789abcdef"[net_info.mac[i]&15];mac[i*3+2]=i==5?0:':';}
 char ip[16],gateway[16],mask[16];sf_net_format_ipv4(net_info.ip,ip);sf_net_format_ipv4(net_info.gateway,gateway);sf_net_format_ipv4(net_info.netmask,mask);
 sf_log("net: mac=");sf_log(mac);sf_log(" ip=");sf_log(ip);sf_log(" gateway=");sf_log(gateway);sf_log(" netmask=");sf_log(mask);
 sf_log(" rx=");sf_number(net_info.rx_frames);sf_log(" tx=");sf_number(net_info.tx_frames);sf_log(" dhcp=");sf_log(net_dhcp==1?"leased":net_dhcp==2?"static":"none");
 sf_log(" irq=");sf_number(m.mode);sf_log(net_info.link?" link=up\n":" link=down\n");
}
/* 主动续租：mode=1 触发 DISCOVER/REQUEST 流程，返回新租约或静态回退。 */
static void net_dhcp_line(void){
 if(!net_ready){sf_log("dhcp: net DOWN\n");return;}
 SfMessage m;sf_message(&m,OP_NET_DHCP,1);int r=net_call(&m);
 if(r||m.length!=sizeof(SfNetInfo)){sf_log("dhcp: failed code=");sf_number(r<0?-r:r);sf_log("\n");return;}
 SfNetInfo info;memcpy(&info,m.data,sizeof(info));net_info=info;net_dhcp=(int)m.mode;
 char ip[16],gateway[16],mask[16];sf_net_format_ipv4(info.ip,ip);sf_net_format_ipv4(info.gateway,gateway);sf_net_format_ipv4(info.netmask,mask);
 sf_log(net_dhcp==1?"dhcp: LEASED ip=":"dhcp: STATIC FALLBACK ip=");sf_log(ip);sf_log(" gateway=");sf_log(gateway);sf_log(" netmask=");sf_log(mask);sf_log("\n");
}
static void net_ping_line(char*line){
 uint32_t ip=0;size_t bytes=strlen(line);
 if(bytes>63||sf_net_parse_ipv4(line,&ip)){sf_log("ping: invalid address\n");return;}
 SfMessage m;sf_message(&m,OP_NET_PING,0);memcpy(m.data,line,bytes);m.data[bytes]=0;m.length=bytes+1;int r=net_call(&m);
 if(r){sf_log("ping: failed code=");sf_number(-r);sf_log("\n");return;}
 sf_log("ping: reply from ");sf_log(line);sf_log(" in ");sf_number((uint64_t)m.mode*10);sf_log(" ms\n");
}
static int parse_port(const char*text,uint16_t*out){
 if(!text||!*text)return SF_INVALID;unsigned value=0;
 while(*text){if(*text<'0'||*text>'9')return SF_INVALID;value=value*10u+(unsigned)(*text-'0');if(value>65535u)return SF_INVALID;text++;}
 if(!value)return SF_INVALID;*out=(uint16_t)value;return SF_OK;
}
static char* split_space(char*text){while(*text&&*text!=' ')text++;return *text?text:0;}
static void net_packet_line(char*line,uint32_t opcode,const char*name){
 char*second=split_space(line);
 if(!second){sf_log(name);sf_log(": expected <ip> <port> <text>\n");return;}
 *second=0;uint32_t ip=0;
 if(sf_net_parse_ipv4(line,&ip)){sf_log(name);sf_log(": invalid address\n");return;}
 char*rest=second+1;while(*rest==' ')rest++;
 char*third=split_space(rest);
 if(!third){sf_log(name);sf_log(": expected <ip> <port> <text>\n");return;}
 *third=0;uint16_t port=0;
 if(parse_port(rest,&port)){sf_log(name);sf_log(": invalid port\n");return;}
 const char*payload=third+1;size_t bytes=strlen(payload);
 if(bytes>SF_NET_PAYLOAD_MAX){sf_log(name);sf_log(": payload too long\n");return;}
 SfNetPacket packet;memset(&packet,0,sizeof(packet));packet.ip=ip;packet.port=port;packet.bytes=(uint16_t)bytes;
 memcpy(packet.data,payload,bytes);
 SfMessage m;sf_message(&m,opcode,0);m.length=sizeof(packet);memcpy(m.data,&packet,sizeof(packet));
 int r=net_call(&m);
 if(r||m.length!=sizeof(packet)){sf_log(name);sf_log(": failed code=");sf_number(r<0?-r:r);sf_log("\n");return;}
 memcpy(&packet,m.data,sizeof(packet));
 if(packet.bytes>SF_NET_PAYLOAD_MAX){sf_log(name);sf_log(": bad reply\n");return;}
 char from[16];sf_net_format_ipv4(packet.ip,from);char text[SF_NET_PAYLOAD_MAX+1];
 memcpy(text,packet.data,packet.bytes);text[packet.bytes]=0;
 sf_log(name);sf_log(": reply from ");sf_log(from);sf_log(":");sf_number(packet.port);sf_log(" \"");sf_log(text);sf_log("\" in ");sf_number((uint64_t)m.mode*10);sf_log(" ms\n");
}
static void net_conn_line(char*line,uint32_t opcode){
 if(opcode==OP_NET_OPEN){
  char*second=split_space(line);if(!second){sf_log("open: expected <ip> <port>\n");return;}
  *second=0;uint32_t ip=0;if(sf_net_parse_ipv4(line,&ip)){sf_log("open: invalid address\n");return;}
  char*rest=second+1;while(*rest==' ')rest++;uint16_t port=0;
  if(parse_port(rest,&port)){sf_log("open: invalid port\n");return;}
  SfNetConnRequest req;memset(&req,0,sizeof(req));req.peer_ip=ip;req.peer_port=port;req.protocol=SF_NET_CONN_TCP;
  SfMessage m;sf_message(&m,opcode,0);m.length=sizeof(req);memcpy(m.data,&req,sizeof(req));int r=net_call(&m);
  if(r||m.length!=sizeof(SfNetConnInfo)){sf_log("open: failed code=");sf_number(r<0?-r:r);sf_log("\n");return;}
  SfNetConnInfo info;memcpy(&info,m.data,sizeof(info));char peer[16];sf_net_format_ipv4(info.peer_ip,peer);
  sf_log("conn: handle=");sf_number(info.handle);sf_log(" peer=");sf_log(peer);sf_log(":");sf_number(info.peer_port);sf_log(" local=");sf_number(info.local_port);sf_log(" state=established\n");
  return;
 }
 char*payload=0;
 if(opcode==OP_NET_SEND){char*second=split_space(line);if(!second){sf_log("send: expected <handle> <text>\n");return;}*second=0;payload=second+1;while(*payload==' ')payload++;}
 unsigned long handle=0;const char*p=line;
 if(!*p){sf_log("conn: expected <handle>\n");return;}
 for(;*p;p++){if(*p<'0'||*p>'9'||handle>0x0fffffffu){sf_log("conn: invalid handle\n");return;}handle=handle*10u+(unsigned)(*p-'0');}
 SfMessage m;sf_message(&m,opcode,0);
 if(opcode==OP_NET_SEND){
  size_t bytes=strlen(payload);if(bytes>SF_NET_CONN_PAYLOAD_MAX){sf_log("send: payload too long\n");return;}
  SfNetConnSend send;memset(&send,0,sizeof(send));send.handle=(uint32_t)handle;send.bytes=(uint32_t)bytes;memcpy(send.payload,payload,bytes);
  m.length=8u+(unsigned)bytes;memcpy(m.data,&send,8u);memcpy(m.data+8,send.payload,bytes);
 } else if(opcode==OP_NET_RECV){
  SfNetConnRecv recv;memset(&recv,0,sizeof(recv));recv.handle=(uint32_t)handle;recv.timeout_ticks=100;m.length=sizeof(recv);memcpy(m.data,&recv,sizeof(recv));
 } else {m.length=sizeof(uint32_t);memcpy(m.data,&handle,sizeof(handle));}
 int r=net_call(&m);
 if(r){sf_log(opcode==OP_NET_RECV?"recv":opcode==OP_NET_SEND?"send":opcode==OP_NET_CLOSE?"close":"conn");sf_log(": failed code=");sf_number(r<0?-r:r);sf_log("\n");return;}
 if(opcode==OP_NET_RECV){char text[SF_NET_CONN_PAYLOAD_MAX+1];unsigned bytes=m.length;if(bytes>SF_NET_CONN_PAYLOAD_MAX){sf_log("recv: bad reply\n");return;}memcpy(text,m.data,bytes);text[bytes]=0;sf_log("conn: recv ");sf_number(bytes);sf_log(" bytes \"");sf_log(text);sf_log("\" state=");sf_log(m.mode==SF_NET_CONN_ESTABLISHED?"established":m.mode==SF_NET_CONN_CLOSED?"closed":"open");sf_log("\n");return;}
 if(m.length!=sizeof(SfNetConnInfo)){sf_log("conn: bad reply\n");return;}
 SfNetConnInfo info;memcpy(&info,m.data,sizeof(info));
 sf_log(opcode==OP_NET_CLOSE?"conn: closed handle=":"conn: handle=");sf_number(info.handle);sf_log(" sent=");sf_number(info.bytes_sent);sf_log(" recv=");sf_number(info.bytes_received);sf_log(" state=");sf_log(info.state==SF_NET_CONN_CLOSED?"closed":"established");sf_log("\n");
}
static void net_conns_line(void){
 if(!net_ready){sf_log("conns: net DOWN\n");return;}
 SfMessage m;sf_message(&m,OP_NET_LIST,0);int r=net_call(&m);
 if(r){sf_log("conns: failed code=");sf_number(-r);sf_log("\n");return;}
 unsigned bytes=m.length;if(bytes>SF_NET_CONN_MAX*sizeof(SfNetConnInfo)||bytes%sizeof(SfNetConnInfo)){sf_log("conns: bad reply\n");return;}
 sf_log("conns: ");sf_number(m.mode);sf_log(" active\n");
 for(unsigned off=0;off<bytes;off+=sizeof(SfNetConnInfo)){
  SfNetConnInfo info;memcpy(&info,m.data+off,sizeof(info));char peer[16];sf_net_format_ipv4(info.peer_ip,peer);
  sf_log("conn: handle=");sf_number(info.handle);sf_log(" peer=");sf_log(peer);sf_log(":");sf_number(info.peer_port);sf_log(" local=");sf_number(info.local_port);
  sf_log(" sent=");sf_number(info.bytes_sent);sf_log(" recv=");sf_number(info.bytes_received);sf_log(" state=");sf_log(info.state==SF_NET_CONN_ESTABLISHED?"established":"closed");sf_log("\n");
 }
}
static void native_forget(void){
 if(runtime_pid)sf_sys(SYS_KILL,runtime_pid,SF_DEVICE_GONE,0);
 if(runtime_ep)sf_sys(SYS_CAP_REVOKE,runtime_ep,0,0);if(runtime_reply)sf_sys(SYS_CAP_REVOKE,runtime_reply,0,0);
 runtime_pid=runtime_ep=runtime_reply=0;
}
static int native_start_service(void){
 unsigned state=manager.modules[runtime_slot].state;if(state!=MOD_VERIFIED&&state!=MOD_STOPPED)return SF_INVALID;
 if(runtime_pid||runtime_ep||runtime_reply)return SF_BUSY;
 int r=sfmod_start(&manager,runtime_slot);if(r)return r;
 int ep=sf_sys(SYS_EP_CREATE,0,0,0);if(ep<0){r=ep;goto fail;}runtime_reply=ep;
 ep=sf_sys(SYS_EP_CREATE,0,0,0);if(ep<0){r=ep;goto fail;}runtime_ep=ep;
 int pid=spawn_package("sf-native.sfmod",runtime_ep,RIGHT_RECV);if(pid<0){r=pid;goto fail;}runtime_pid=pid;return 0;
fail:native_forget();sfmod_prepare(&manager,runtime_slot);sfmod_stop(&manager,runtime_slot);return r;
}
static int native_stop_service(void){
 int prepared=0,r=0;unsigned state=manager.modules[runtime_slot].state;
 if(state==MOD_STOPPED&&!runtime_pid&&!runtime_ep&&!runtime_reply)return 0;
 if(state==MOD_ACTIVE){r=sfmod_prepare(&manager,runtime_slot);if(r)return r;prepared=1;}
 else if(state!=MOD_DRAINING)return SF_INVALID;
 r=sfmod_stop(&manager,runtime_slot);if(r){if(prepared)sfmod_cancel(&manager,runtime_slot);return r;}
 native_forget();native_draining=0;return 0;
}
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
static void echo_supervise(void){
 uint64_t now=sf_ticks();int failed=0;
 if(echo_watch.state==SF_SUP_RUNNING){
  failed=sf_sys(SYS_TASK_STATUS,echo_pid,0,0)!=SF_TASK_RUNNING;
  if(!failed&&now>=echo_health_at){
   echo_health_at=echo_after(now,100);if(echo_request(echo_ep,echo_pid,100)){failed=1;sf_log("echo: health failure\n");}now=sf_ticks();
  }
  if(failed){
   if(echo_stop_service()){echo_watch.state=SF_SUP_BLOCKED;echo_watch.next_tick=0;sf_log("echo: recovery blocked by cleanup\n");return;}
   sf_supervisor_failure(&echo_watch,now);sf_log(echo_watch.state==SF_SUP_BLOCKED?"echo: recovery blocked\n":"echo: recovery scheduled\n");
  }
 }
 if(echo_watch.state==SF_SUP_WAITING&&!sf_supervisor_begin(&echo_watch,now)){
  int r=echo_start_service();now=sf_ticks();
  if(r){sf_supervisor_failure(&echo_watch,now);sf_log(echo_watch.state==SF_SUP_BLOCKED?"echo: recovery blocked\n":"echo: recovery retry scheduled\n");}
  else{sf_supervisor_started(&echo_watch,now);echo_health_at=echo_after(now,100);sf_log("echo: recovered\n");}
 }
}
static void echo_status(void){
 static const char*names[]={"STOPPED","WAITING","STARTING","RUNNING","BLOCKED"};uint64_t now=sf_ticks();
 sf_log("echo: state=");sf_log(echo_watch.state<5?names[echo_watch.state]:"UNKNOWN");sf_log(" attempts=");sf_number(echo_watch.attempts);
 sf_log(echo_watch.state==SF_SUP_RUNNING&&now>=echo_watch.running_since&&now-echo_watch.running_since>=echo_watch.stable_ticks?" stable=1\n":" stable=0\n");
}
static int native_ping(void){
 SfMessage m;sf_message(&m,OP_NATIVE_HEALTH,++requests);int r=sf_send(runtime_ep,&m);if(r)return r;
 long from=sf_recv(runtime_reply,&m,sf_deadline(100));
 return from==runtime_pid&&m.h.request_id==requests&&m.h.opcode==OP_NATIVE_HEALTH&&!m.status&&!m.length?0:SF_INVALID;
}
static void native_supervise(void){
 uint64_t now=sf_ticks();int failed=0;
 if(native_watch.state==SF_SUP_RUNNING){
  failed=sf_sys(SYS_TASK_STATUS,runtime_pid,0,0)!=SF_TASK_RUNNING;
  if(!failed&&now>=native_health_at){native_health_at=echo_after(now,100);if(native_ping()){failed=1;sf_log("native: health failure\n");}now=sf_ticks();}
  if(failed){
   if(sfmod_prepare(&manager,runtime_slot)){native_watch.state=SF_SUP_BLOCKED;sf_log("native: recovery blocked by prepare\n");return;}
   native_forget();native_draining=1;sf_supervisor_failure(&native_watch,now);
   sf_log(native_watch.state==SF_SUP_BLOCKED?"native: recovery blocked\n":"native: recovery scheduled\n");
  }
 }
 if(native_draining){
  int r=sfmod_stop(&manager,runtime_slot);if(r==SF_DEPENDENCY||r==SF_BUSY)return;
  native_draining=0;if(r){native_watch.state=SF_SUP_BLOCKED;sf_log("native: recovery blocked by cleanup\n");return;}
 }
 if(native_watch.state==SF_SUP_WAITING&&!sf_supervisor_begin(&native_watch,now)){
  int r=native_start_service();now=sf_ticks();
  if(r){sf_supervisor_failure(&native_watch,now);sf_log(native_watch.state==SF_SUP_BLOCKED?"native: recovery blocked\n":"native: recovery retry scheduled\n");}
  else{sf_supervisor_started(&native_watch,now);native_health_at=echo_after(now,100);sf_log("native: recovered\n");}
 }
}
static void native_status(void){
 static const char*names[]={"STOPPED","WAITING","STARTING","RUNNING","BLOCKED"};uint64_t now=sf_ticks();
 sf_log("native: state=");sf_log(native_watch.state<5?names[native_watch.state]:"UNKNOWN");sf_log(" attempts=");sf_number(native_watch.attempts);
 sf_log(native_watch.state==SF_SUP_RUNNING&&now>=native_watch.running_since&&now-native_watch.running_since>=native_watch.stable_ticks?" stable=1":" stable=0");
 sf_log(" leases=");sf_number(manager.modules[runtime_slot].resources[RES_LEASE]);sf_log(manager.modules[runtime_slot].state==MOD_DRAINING?" draining=1\n":" draining=0\n");
}
static Child*child(uint32_t pid){for(unsigned i=0;i<16;i++)if(children[i].pid==pid)return&children[i];return 0;}
static int session_identity(uint32_t*uid,uint32_t*gid,uint32_t*owner){
 const SfSession*s=sf_session_current(&sessions);if(!s)return SF_DENIED;
 if(uid)*uid=s->uid;if(gid)*gid=s->gid;if(owner)*owner=sf_session_owner(sf_session_active(&sessions));return 0;
}
static int shell_open(const char*path,unsigned mode){uint32_t uid,gid,owner;int r=session_identity(&uid,&gid,&owner);return r?r:sf_ns_open_as(&ns,owner,"/",path,mode,uid,gid);}
static int launch_app_root_args(const char*name,const char*root,const SfArguments*arguments){
 if(!strcmp(name,"installed"))name="/installed.sfk";
 int slot=-1;for(unsigned i=0;i<16;i++)if(!children[i].pid){slot=i;break;}if(slot<0)return SF_NO_SPACE;
 uint32_t uid,gid,owner;int ir=session_identity(&uid,&gid,&owner);if(ir)return ir;
 /* Reserve and pin the directory before creating a child. No failure after
  * spawn can leave an unregistered child's RPC in the manager queue. */
 int directory=root?sf_ns_open_as(&ns,owner,"/",root,OPEN_READ|OPEN_DIRECTORY,uid,gid):0;if(directory<0)return directory;
 int ep=0,r=sfmod_acquire(&manager,runtime_slot,RES_LEASE);if(r)goto fail_root;
 ep=sf_sys(SYS_EP_CREATE,0,0,0);if(ep<0){r=ep;goto fail_lease;}
 int pid=spawn_package_policy(name,ep,RIGHT_RECV,0,0,directory!=0,arguments,0,2);if(pid<0){r=pid;goto fail_lease;}
 if(directory)ns.handles[(directory&255)-1].owner=pid;
 children[slot]=(Child){pid,ep,directory,1,uid,gid};return pid;
fail_lease:if(ep>0)sf_sys(SYS_CAP_REVOKE,ep,0,0);sfmod_release(&manager,runtime_slot,RES_LEASE);
fail_root:if(directory)sf_ns_close(&ns,owner,directory);return r;
}
static int launch_app_root(const char*name,const char*root){return launch_app_root_args(name,root,0);}
static int launch_app(const char*name){return launch_app_root(name,0);}
static void app_reap(uint32_t pid){Child*c=child(pid);if(!c)return;sf_ns_reap(&ns,pid);sf_sys(SYS_CAP_REVOKE,c->reply,0,0);if(c->lease)sfmod_release(&manager,runtime_slot,RES_LEASE);memset(c,0,sizeof(*c));}
static int serve_once(unsigned timeout_ms){
 /* sf-fs owns the disk format layer but never touches virtio-blk; while the
  * init waits for file RPC it also answers the module's block requests. */
 sf_storage_broker(1);
 SfMessage m;long sender=sf_recv(fs_ep,&m,sf_deadline(timeout_ms));if(sender<0)return sender;Child*c=child(sender);if(!c)return SF_INVALID;
 if(m.h.opcode==OP_HELLO)return m.status;
 sf_file_dispatch_as(&ns,sender,&c->root,&m,c->uid,c->gid);return sf_send(c->reply,&m);
}
static int wait_app(uint32_t pid){uint64_t end=sf_ticks()+300;for(;;){long status=sf_sys(SYS_TASK_STATUS,pid,0,0);if(status!=SF_TASK_RUNNING){app_reap(pid);return status;}if(sf_ticks()>end){sf_sys(SYS_KILL,pid,SF_TIMEOUT,0);app_reap(pid);return SF_TIMEOUT;}serve_once(10);sf_yield();}}
static int echo_call(uint32_t,uint32_t);
static int channel_test(int echo_check){
 int a=launch_app_root("readprobe.sfk","/disk/etc"),b=launch_app_root("readprobe.sfk","/disk/etc");int er=echo_check?echo_call(echo_ep,echo_pid):0;int ar=a>0?wait_app(a):a,br=b>0?wait_app(b):b;
 if(!er&&!ar&&!br)sf_log(echo_check?"ECHO CHANNEL TEST PASSED\n":"RPC CHANNEL TEST PASSED\n");else{sf_log(echo_check?"echo-channels: failed e=":"rpc-channels: failed e=");sf_number(er<0?-(int64_t)er:er);sf_log(" a=");sf_number(ar<0?-(int64_t)ar:ar);sf_log(" b=");sf_number(br<0?-(int64_t)br:br);sf_log("\n");}return er?er:ar?ar:br;
}
static int echo_request(uint32_t ep,uint32_t pid,unsigned timeout_ms){if(manager.modules[echo_slot].state!=MOD_ACTIVE)return SF_DRAINING;EchoPeer*p=echo_peer(pid);if(!p||p->service!=ep)return SF_STALE_HANDLE;if(sf_sys(SYS_TASK_STATUS,pid,0,0)!=SF_TASK_RUNNING)return SF_DEVICE_GONE;SfMessage m;sf_message(&m,OP_ECHO,++requests);memcpy(m.data,"ping",4);m.length=4;int r=sf_send(ep,&m);if(r)return r;long from=sf_recv(p->reply,&m,sf_deadline(timeout_ms));return from==pid&&m.h.request_id==requests&&m.h.opcode==OP_ECHO&&!m.status&&m.length==4&&!memcmp(m.data,"ping",4)?0:SF_INVALID;}
static int echo_call(uint32_t ep,uint32_t pid){return echo_request(ep,pid,1000);}
static void echo_capacity_test(void){
 sf_assert(!echo_stop_service(),"echo capacity test stops current instance");int held[64],used=0,r=0;
 while(used<64&&(r=sf_sys(SYS_EP_CREATE,0,0,0))>0)held[used++]=r;
 sf_assert(r==SF_NO_SPACE&&used>0,"echo capacity test exhausts actual endpoint capability slots");
 sf_assert(echo_start_service()==SF_NO_SPACE&&manager.modules[echo_slot].state==MOD_STOPPED,"failed echo service allocation rolls back lifecycle");
 sf_assert(!sf_sys(SYS_CAP_REVOKE,held[--used],0,0),"echo capacity test releases one slot");
 sf_assert(echo_start_service()==SF_NO_SPACE&&manager.modules[echo_slot].state==MOD_STOPPED,"failed echo reply allocation reclaims service endpoint");
 while(used)sf_assert(!sf_sys(SYS_CAP_REVOKE,held[--used],0,0),"echo capacity test reclaims temporary endpoint");
 sf_assert(!echo_start_service()&&!echo_call(echo_ep,echo_pid),"echo restarts after endpoint allocation failures");sf_supervisor_enable(&echo_watch,sf_ticks(),1);echo_health_at=echo_after(sf_ticks(),100);sf_log("ECHO CAPACITY TEST PASSED\n");
}
static void native_capacity_test(void){
 sf_assert(!native_stop_service(),"native capacity test stops current instance");int held[64],used=0,r=0;
 while(used<64&&(r=sf_sys(SYS_EP_CREATE,0,0,0))>0)held[used++]=r;
 sf_assert(r==SF_NO_SPACE&&used>0,"native capacity test exhausts root capability slots");
 sf_assert(native_start_service()==SF_NO_SPACE&&manager.modules[runtime_slot].state==MOD_STOPPED&&!runtime_reply&&!runtime_ep&&!runtime_pid,"native reply allocation failure rolls back registration");
 sf_assert(!sf_sys(SYS_CAP_REVOKE,held[--used],0,0),"native capacity releases one root slot");
 sf_assert(native_start_service()==SF_NO_SPACE&&manager.modules[runtime_slot].state==MOD_STOPPED&&!runtime_reply&&!runtime_ep&&!runtime_pid,"native service allocation failure reclaims reply");
 while(used)sf_assert(!sf_sys(SYS_CAP_REVOKE,held[--used],0,0),"native capacity releases held endpoint");
 int shared=sf_sys(SYS_EP_CREATE,0,0,0);sf_assert(shared>0,"native capacity creates shared idle receiver");uint32_t pids[16];unsigned count=0;
 while(count<16&&(r=spawn_echo("echo.sfmod",shared,RIGHT_RECV))>0)pids[count++]=r;
 sf_assert(r==SF_NO_SPACE&&count>0,"native capacity fills actual process resources");
 int first=sf_sys(SYS_EP_CREATE,0,0,0),second=sf_sys(SYS_EP_CREATE,0,0,0);sf_assert(first>0&&second>0,"native process capacity test still has both root endpoint slots");
 sf_sys(SYS_CAP_REVOKE,first,0,0);sf_sys(SYS_CAP_REVOKE,second,0,0);
 sf_assert(native_start_service()==SF_NO_SPACE&&manager.modules[runtime_slot].state==MOD_STOPPED&&!runtime_reply&&!runtime_ep&&!runtime_pid,"failed native process creation reclaims both endpoints");
 while(count){uint32_t pid=pids[--count];sf_sys(SYS_KILL,pid,0,0);echo_forget(pid);}sf_sys(SYS_CAP_REVOKE,shared,0,0);
 sf_assert(!native_start_service(),"native starts after real capability and process exhaustion");sf_supervisor_enable(&native_watch,sf_ticks(),1);native_health_at=echo_after(sf_ticks(),100);sf_log("NATIVE CAPACITY TEST PASSED\n");
}
static void native_drain_test(void){
 int a=launch_app_root("readprobe.sfk","/disk/etc"),b=launch_app_root("readprobe.sfk","/disk/etc");sf_assert(a>0&&b>0,"native drain holds two real application leases");
 sf_assert(native_stop_service()==SF_DEPENDENCY&&manager.modules[runtime_slot].state==MOD_ACTIVE,"busy explicit native stop preserves active provider");
 sf_assert(!sf_sys(SYS_KILL,runtime_pid,SF_DEVICE_GONE,0),"native drain terminates actual runtime");native_supervise();
 sf_assert(manager.modules[runtime_slot].state==MOD_DRAINING&&manager.modules[runtime_slot].resources[RES_LEASE]==2&&native_draining,"failed native waits for existing application leases");
 sf_assert(launch_app("hello.sfk")==SF_DRAINING,"failed native drain rejects new launches");
 int ar=wait_app(a),br=wait_app(b);sf_assert(!ar&&!br&&!manager.modules[runtime_slot].resources[RES_LEASE],"existing apps finish namespace RPC and release failed runtime leases");sf_log("NATIVE DRAIN TEST PASSED\n");
}
/* Read-only system projections: JSON-free text generated at read time from
 * authorized kernel snapshots. They add no new kernel authority and consume
 * no cached payload, so /proc and /sys stay small and revocable. */
#define PROJECTION_CAP 1024
enum { PROJECTION_UPTIME=1,PROJECTION_IPC_STATS,PROJECTION_SELF_STATUS,PROJECTION_CHILDREN,
 PROJECTION_MODULES=10,PROJECTION_DEVICES,PROJECTION_DMA,
 PROJECTION_NET_INFO=20,PROJECTION_NET_CONNECTIONS };
static uint8_t projection_payload[PROJECTION_CAP];static size_t projection_used;
static void projection_reset(void){projection_used=0;}
static void projection_char(char c){if(projection_used<PROJECTION_CAP)projection_payload[projection_used++]=(uint8_t)c;}
static void projection_put(const char*s){while(*s)projection_char(*s++);}
static void projection_number(uint64_t v){char digits[20];unsigned n=0;if(!v){projection_char('0');return;}while(v&&n<sizeof(digits)){digits[n++]=(char)('0'+v%10);v/=10;}while(n)projection_char(digits[--n]);}
static void projection_hex(uint64_t v,unsigned width){static const char*digits="0123456789abcdef";for(unsigned i=width;i;i--)projection_char(digits[(v>>((i-1)*4))&15]);}
static void projection_hex64(uint64_t v){projection_put("0x");projection_hex(v,16);}
static const char*module_state_name(unsigned state){static const char*names[]={"discovered","verified","resolved","staged","initialized","active","quiescing","draining","stopped","unloaded","failed","quarantined"};return state<sizeof(names)/sizeof(*names)?names[state]:"unknown";}
static void projection_task(uint32_t pid){
 projection_put("pid=");projection_number(pid);long status=sf_sys(SYS_TASK_STATUS,pid,0,0);
 if(status==SF_TASK_RUNNING)projection_put(" status=running\n");
 else if(status>=0){projection_put(" status=exited code=");projection_number((uint64_t)status);projection_put("\n");}
 else{projection_put(" status=gone code=");projection_number((uint64_t)-status);projection_put("\n");}
}
static void projection_add_pid(uint32_t*pids,unsigned*count,uint32_t pid){
 if(!pid||pid==1||*count>=16)return;for(unsigned i=0;i<*count;i++)if(pids[i]==pid)return;pids[(*count)++]=pid;
}
static void projection_build_uptime(void){
 uint64_t ticks=sf_ticks();projection_put("ticks=");projection_number(ticks);projection_put("\nhz=100\nseconds=");projection_number(ticks/100);projection_char('.');uint64_t fraction=ticks%100;if(fraction<10)projection_char('0');projection_number(fraction);projection_put("\n");
}
static void projection_build_ipc(void){
 SfIpcStats stats;memset(&stats,0,sizeof(stats));long r=sf_ipc_stats(&stats);
 if(r){projection_put("error=");projection_number((uint64_t)-r);projection_put("\n");return;}
 projection_put("version=");projection_number(stats.version);projection_put("\nsend_bytes=");projection_number(stats.send_bytes);projection_put("\nreceive_bytes=");projection_number(stats.receive_bytes);projection_put("\n");
}
static void projection_build_self(void){
 long state=sf_sys(SYS_TASK_STATUS,1,0,0);long priority=sf_sys(SYS_TASK_PRIORITY,1,0,SF_PRIORITY_QUERY);
 long pages_limit=sf_task_page_limit(1,0,SF_LIMIT_QUERY),pages_used=sf_task_page_usage(1);
 long caps_limit=sf_sys(SYS_TASK_CAP_LIMIT,1,0,SF_LIMIT_QUERY),caps_used=sf_task_cap_usage(1);
 projection_put("pid=1\nstate=");projection_put(state==SF_TASK_RUNNING?"running":"stopped");
 projection_put("\npriority=");projection_number(priority<0?0:(uint64_t)priority);
 projection_put("\npages_used=");projection_number(pages_used<0?0:(uint64_t)pages_used);
 projection_put("\npages_limit=");projection_number(pages_limit<0?0:(uint64_t)pages_limit);
 projection_put("\ncaps_used=");projection_number(caps_used<0?0:(uint64_t)caps_used);
 projection_put("\ncaps_limit=");projection_number(caps_limit<0?0:(uint64_t)caps_limit);projection_put("\n");
}
static void projection_build_children(void){
 uint32_t pids[16];unsigned count=0;projection_add_pid(pids,&count,echo_pid);projection_add_pid(pids,&count,runtime_pid);projection_add_pid(pids,&count,net_pid);
 for(unsigned i=0;i<sizeof(children)/sizeof(*children);i++)projection_add_pid(pids,&count,children[i].pid);
 projection_put("pid=1 status=running\n");for(unsigned i=0;i<count;i++)projection_task(pids[i]);
}
static void projection_build_modules(void){
 for(unsigned i=0;i<16;i++){
  const SfModule*m=&manager.modules[i];if(!m->id[0]||m->state==MOD_DISCOVERED||m->state==MOD_UNLOADED)continue;
  projection_put("module=");projection_put(m->id);projection_put(" state=");projection_put(module_state_name(m->state));
  projection_put(" generation=");projection_number(m->generation);projection_put(" pinned=");projection_char(m->pinned?'1':'0');
  projection_put(" dependencies=");projection_hex(m->dependencies,8);projection_put("\n");
 }
}
static void projection_build_devices(void){
 for(unsigned bdf=0;bdf<256&&projection_used+48<PROJECTION_CAP;bdf++){
  long id=sf_sys(SYS_PCI_READ,bdf,0,0);if(id<0||(uint32_t)id==0xffffffffu)continue;
  projection_put("pci=");projection_number(bdf>>3);projection_char('.');projection_number(bdf&7);
  projection_put(" vendor=0x");projection_hex((uint32_t)id&0xffff,4);
  projection_put(" device=0x");projection_hex(((uint32_t)id>>16)&0xffff,4);projection_put("\n");
 }
}
static void projection_build_dma(void){
 SfDmaStatus status;long r=sf_sys(SYS_DMA_STATUS,(uint64_t)&status,sizeof(status),0);
 if(r){projection_put("error=");projection_number((uint64_t)-r);projection_put("\n");return;}
 projection_put("mode=");projection_number(status.mode);projection_put("\nowner=");projection_number(status.owner);
 projection_put("\nguard=");projection_number(status.guard_intact);projection_put("\nactive_mapping=");projection_number(status.active_mapping);
 projection_put("\nfaults=");projection_number(status.faults);projection_put("\nsource=");projection_number(status.source);
 projection_put("\nreason=");projection_number(status.reason);projection_put("\nlast_address=");projection_hex64(status.last_address);
 projection_put("\nprobe_address=");projection_hex64(status.probe_address);projection_put("\n");
}
static void projection_build_net_info(void){
 if(!net_ready){projection_put("state=down\n");return;}
 SfMessage m;sf_message(&m,OP_NET_INFO,0);SfNetInfo info=net_info;
 if(!net_call(&m)&&m.length==sizeof(SfNetInfo)){memcpy(&info,m.data,sizeof(info));net_info=info;}
 char mac[18],ip[16],gateway[16],mask[16];
 for(unsigned i=0;i<6;i++){mac[i*3]="0123456789abcdef"[info.mac[i]>>4];mac[i*3+1]="0123456789abcdef"[info.mac[i]&15];mac[i*3+2]=i==5?0:':';}
 sf_net_format_ipv4(info.ip,ip);sf_net_format_ipv4(info.gateway,gateway);sf_net_format_ipv4(info.netmask,mask);
 projection_put("state=up\nmac=");projection_put(mac);projection_put("\nip=");projection_put(ip);
 projection_put("\ngateway=");projection_put(gateway);projection_put("\nnetmask=");projection_put(mask);
 projection_put("\ndhcp=");projection_put(net_dhcp==1?"leased":net_dhcp==2?"static":"none");
 projection_put("\nrx=");projection_number(info.rx_frames);projection_put(" tx=");projection_number(info.tx_frames);projection_put("\n");
}
static void projection_build_net_connections(void){
 if(!net_ready){projection_put("connections=0\n");return;}
 SfMessage m;sf_message(&m,OP_NET_LIST,0);int r=net_call(&m);
 if(r){projection_put("error=");projection_number(-r);projection_put("\n");return;}
 unsigned bytes=m.length;if(bytes>SF_NET_CONN_MAX*sizeof(SfNetConnInfo)||bytes%sizeof(SfNetConnInfo)){projection_put("error=");projection_number(SF_BAD_FORMAT);projection_put("\n");return;}
 projection_put("connections=");projection_number(m.mode);projection_put("\n");
 for(unsigned off=0;off<bytes;off+=sizeof(SfNetConnInfo)){
  SfNetConnInfo info;memcpy(&info,m.data+off,sizeof(info));char peer[16];sf_net_format_ipv4(info.peer_ip,peer);
  projection_put("conn handle=");projection_number(info.handle);projection_put(" peer=");projection_put(peer);projection_char(':');
  projection_number(info.peer_port);projection_put(" local=");projection_number(info.local_port);
  projection_put(" sent=");projection_number(info.bytes_sent);projection_put(" recv=");projection_number(info.bytes_received);
  projection_put(" state=");projection_put(info.state==SF_NET_CONN_ESTABLISHED?"established":"closed");projection_put("\n");
 }
}
static void projection_build(unsigned id){
 projection_reset();switch(id){
  case PROJECTION_UPTIME:projection_build_uptime();break;
  case PROJECTION_IPC_STATS:projection_build_ipc();break;
  case PROJECTION_SELF_STATUS:projection_build_self();break;
  case PROJECTION_CHILDREN:projection_build_children();break;
  case PROJECTION_MODULES:projection_build_modules();break;
  case PROJECTION_DEVICES:projection_build_devices();break;
  case PROJECTION_DMA:projection_build_dma();break;
  case PROJECTION_NET_INFO:projection_build_net_info();break;
  case PROJECTION_NET_CONNECTIONS:projection_build_net_connections();break;
  default:projection_put("error=");projection_number(SF_NOT_FOUND);projection_put("\n");break;
 }
}
static long projection_read(unsigned id,uint64_t offset,void*out,size_t bytes){
 if(!out)return SF_INVALID;projection_build(id);if(offset>=projection_used)return 0;
 size_t available=projection_used-(size_t)offset;if(bytes>available)bytes=available;memcpy(out,projection_payload+offset,bytes);return(long)bytes;
}
static void projection_setup(void){
 sf_assert(!sf_ns_create(&ns,"/proc/self",NODE_DIR,0),"process projection directory");
 sf_assert(!sf_ns_project(&ns,"/proc/uptime",PROJECTION_UPTIME)&&!sf_ns_project(&ns,"/proc/ipc-stats",PROJECTION_IPC_STATS),"process projections");
 sf_assert(!sf_ns_project(&ns,"/proc/self/status",PROJECTION_SELF_STATUS)&&!sf_ns_project(&ns,"/proc/children",PROJECTION_CHILDREN),"process projections");
 sf_assert(!sf_ns_project(&ns,"/sys/modules",PROJECTION_MODULES)&&!sf_ns_project(&ns,"/sys/devices",PROJECTION_DEVICES)&&!sf_ns_project(&ns,"/sys/dma",PROJECTION_DMA),"system projections");
 sf_assert(!sf_ns_project(&ns,"/net/info",PROJECTION_NET_INFO)&&!sf_ns_project(&ns,"/net/connections",PROJECTION_NET_CONNECTIONS),"network projections");
 ns.projection_read=projection_read;
 sf_assert(!sf_ns_protect(&ns,"/proc")&&!sf_ns_protect(&ns,"/proc/self")&&!sf_ns_protect(&ns,"/sys")&&!sf_ns_protect(&ns,"/net"),"system projection directories are read-only");
}
static void namespace_setup(void){
 sf_ns_init(&ns,console_write);const char*dirs[]={"/boot","/system","/modules","/bin","/dev","/proc","/sys","/net","/run","/etc","/home","/var","/tmp"};
 for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++)sf_assert(!sf_ns_create(&ns,dirs[i],NODE_DIR,0),"namespace directory");
 sf_ns_create(&ns,"/dev/console",NODE_CONSOLE,0);sf_ns_create(&ns,"/tmp/notes",NODE_FILE,0);sf_ns_create(&ns,"/run/pipe",NODE_PIPE,0);
 /* Shared endpoints stay usable by every account: the console and pipe are
  * host-facing devices, and /tmp has no sticky bit in this bounded model. */
 sf_assert(!sf_ns_assign(&ns,"/tmp",0,0,0777),"shared temporary directory is writable");
 sf_assert(!sf_ns_assign(&ns,"/dev/console",0,0,0666),"console is readable and writable by accounts");
 sf_assert(!sf_ns_assign(&ns,"/run/pipe",0,0,0666),"pipe is readable and writable by accounts");
 int h=sf_ns_open(&ns,1,"/","/tmp/notes",3);sf_ns_write(&ns,1,h,"SpiritFoxOS ramfs\n",18);sf_ns_close(&ns,1,h);
 projection_setup();
}
static void identity_setup(void){
 sf_assert(!sf_accounts_load_builtin(&accounts),"embedded account database parses");
 sf_session_init(&sessions);
 const SfAccount*root=0;
 sf_assert(!sf_account_lookup(&accounts,"root",&root)&&root->uid==0&&root->gid==0,"account database contains uid-0 root");
 for(unsigned i=0;i<accounts.count;i++){
  const SfAccount*a=&accounts.accounts[i];char home[SF_ACCOUNT_HOME_MAX+1];
  sf_assert(!sf_account_home(a,home,sizeof(home)),"account home path is bounded");
  sf_assert(!sf_ns_create(&ns,home,NODE_DIR,0),"account home exists");
  sf_assert(!sf_ns_assign(&ns,home,a->uid,a->gid,0700),"account home is private to its owner");
 }
 sf_assert(!sf_session_boot(&sessions,root),"console boot session is authenticated root");
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
 int ep=sf_sys(SYS_EP_CREATE,0,0,0);SfMessage message;sf_message(&message,OP_ECHO,1);message.h.cap_count=2;sf_assert(sf_send(ep,&message)==SF_INVALID,"message carries at most one capability");message.h.cap_count=1;sf_assert(sf_send(ep,&message)==SF_DENIED,"capability transfer requires a valid delegatable handle");message.h.cap_count=0;
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
 sf_sys(SYS_KILL,runtime_pid,SF_DEVICE_GONE,0);sf_assert(launch_app("hello.sfk")==SF_DEVICE_GONE&&!echo_call(echo_ep,echo_pid),"runtime death is diagnosed without harming other services");uint32_t old_reply=runtime_reply;sf_assert(!native_stop_service()&&!native_start_service(),"runtime restarts after failure");sf_assert(runtime_reply&&runtime_reply!=old_reply,"runtime reply endpoint recreated after failure");
}
static int installed_preflight(const char*root){
 unsigned state=manager.modules[runtime_slot].state;
 if(state==MOD_QUIESCING||state==MOD_DRAINING)return SF_DRAINING;
 if(state!=MOD_ACTIVE)return SF_STALE_HANDLE;
 if(!runtime_pid||!runtime_ep||!runtime_reply||sf_sys(SYS_TASK_STATUS,runtime_pid,0,0)!=SF_TASK_RUNNING)return SF_DEVICE_GONE;
 if(root){
  uint8_t*snapshot=sf_storage_scratch();long n=sf_persist_preview(snapshot,SF_READFS_FILE_MAX);SfPackage p;
  /* Only a valid package's unsupported invocation is rejected here. A bad
   * package still enters the existing trial/load/rollback path below. */
  if(n>=0&&!sf_package_open_trusted(&p,snapshot,n,&sf_trust_store)&&p.kind==2&&sf_manifest_capability(&p,"namespace.read")==0)return SF_DENIED;
 }
 return 0;
}
static void run_command(const char*name,const char*root,const SfArguments*arguments){
 int installed=!strcmp(name,"installed")||!strcmp(name,"/installed.sfk"),status=0;uint32_t uid,gid,owner;
 int ir=session_identity(&uid,&gid,&owner);if(ir)status=ir;
 else if(root){int h=shell_open(root,OPEN_READ|OPEN_DIRECTORY);if(h<0)status=h;else sf_ns_close(&ns,owner,h);}
 if(!status&&installed&&!sf_session_is_root(&sessions))status=SF_DENIED;
 if(!status&&installed)status=installed_preflight(root);
 if(!status){int trial=installed?sf_persist_begin():0;status=trial;
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
static void namespace_mutate_line(char*line,int directory){
 char*words[1];int count=sf_arguments_split(line,words,1),r=SF_INVALID;uint32_t uid,gid,owner;
 if(session_identity(&uid,&gid,&owner))r=SF_DENIED;
 else if(count==1&&words[0][0]=='/'&&strlen(words[0])<=95){
  char*path=words[0];if(directory==1)r=sf_ns_create_owned(&ns,path,NODE_DIR,0,uid,gid,0755);
  else{size_t cut=strlen(path);while(cut&&path[cut-1]!='/')cut--;
   if(cut&&path[cut]){char parent[96];size_t length=cut==1?1:cut-1;memcpy(parent,path,length);parent[length]=0;
    int h=sf_ns_open_as(&ns,owner,"/",parent,OPEN_WRITE|OPEN_DIRECTORY,uid,gid);
    r=h<0?h:directory==2?sf_ns_rmdir_as(&ns,owner,h,path+cut,uid,gid):sf_ns_unlink_as(&ns,owner,h,path+cut,uid,gid);if(h>=0)sf_ns_close(&ns,owner,h);
   }
  }
 }
 sf_log(directory==2?"rmdir: ":directory?"mkdir: ":"rm: ");if(!r)sf_log("OK\n");else{sf_log("failed code=");sf_number(-r);sf_log("\n");}
}
static void namespace_stat_line(char*line){
 char*words[1];int count=sf_arguments_split(line,words,1),r=SF_INVALID;SfFileStat info;uint32_t uid,gid,owner;
 if(session_identity(&uid,&gid,&owner))r=SF_DENIED;
 else if(count==1&&words[0][0]=='/'&&strlen(words[0])<=95){
  int h=shell_open(words[0],OPEN_READ);r=h<0?h:sf_ns_stat(&ns,owner,h,&info);if(h>=0)sf_ns_close(&ns,owner,h);
 }
 if(r){sf_log("stat: failed code=");sf_number(-r);sf_log("\n");return;}
 sf_log("stat: type=");sf_number(info.type);sf_log(" size=");sf_number(info.size);
 sf_log(" offset=");sf_number(info.offset);sf_log(" mode=");sf_number(info.mode);sf_log(" flags=");sf_number(info.flags);sf_log("\n");
}
static int decimal_u32(const char*s,uint32_t*out){
 if(!*s)return 0;uint32_t value=0;
 for(;*s;s++){if(*s<'0'||*s>'9')return 0;unsigned digit=(unsigned)(*s-'0');if(value>(UINT32_MAX-digit)/10)return 0;value=value*10+digit;}
 *out=value;return 1;
}
static void priority_line(char*line){
 char*words[2];int count=sf_arguments_split(line,words,2);uint32_t pid=0,level=0;long r=SF_INVALID;
 if((count==1||count==2)&&decimal_u32(words[0],&pid)&&(count==1||decimal_u32(words[1],&level)))
  r=sf_sys(SYS_TASK_PRIORITY,pid,level,count==1?SF_PRIORITY_QUERY:SF_PRIORITY_SET);
 if(r<0){sf_log("priority: failed code=");sf_number(-r);sf_log("\n");return;}
 sf_log("priority: pid=");sf_number(pid);sf_log(" level=");sf_number(r);sf_log("\n");
}
static void priority_test(void){
 int a=launch_app("spinner.sfk"),b=launch_app("spinner.sfk");sf_assert(a>0&&b>0,"priority test owns two noncooperating tasks");
 sf_assert(sf_sys(SYS_TASK_PRIORITY,a,0,SF_PRIORITY_QUERY)==SF_PRIORITY_DEFAULT&&sf_sys(SYS_TASK_PRIORITY,b,0,SF_PRIORITY_QUERY)==SF_PRIORITY_DEFAULT,"new tasks start at default priority");
 sf_assert(sf_sys(SYS_TASK_PRIORITY,a,SF_PRIORITY_MAX,SF_PRIORITY_SET)==SF_PRIORITY_MAX&&sf_sys(SYS_TASK_PRIORITY,b,SF_PRIORITY_MAX,SF_PRIORITY_SET)==SF_PRIORITY_MAX,"root raises both timer-preempted spinners");
 sf_assert(sf_sys(SYS_TASK_PRIORITY,1,SF_PRIORITY_MIN,SF_PRIORITY_SET)==SF_PRIORITY_MIN,"root lowers itself for aging test");
 uint64_t before=sf_ticks();while(sf_ticks()-before<40)sf_yield();
 sf_assert(sf_sys(SYS_TASK_STATUS,a,0,0)==SF_TASK_RUNNING&&sf_sys(SYS_TASK_STATUS,b,0,0)==SF_TASK_RUNNING,"lowest-priority manager progresses while both high tasks remain alive");
 sf_assert(sf_sys(SYS_TASK_PRIORITY,1,SF_PRIORITY_DEFAULT,SF_PRIORITY_SET)==SF_PRIORITY_DEFAULT,"root restores normal priority");
 sf_assert(!sf_sys(SYS_KILL,a,0,0)&&!sf_sys(SYS_KILL,b,0,0),"priority test stops owned tasks");app_reap(a);app_reap(b);
 sf_assert(sf_sys(SYS_TASK_PRIORITY,a,0,SF_PRIORITY_QUERY)==SF_NOT_FOUND&&sf_sys(SYS_TASK_PRIORITY,b,0,SF_PRIORITY_QUERY)==SF_NOT_FOUND,"priority queries reject retired tasks");
 sf_log("PRIORITY SCHEDULER TEST PASSED\n");
}
static int ipc_stats_read(SfIpcStats*s){long r=sf_ipc_stats(s);return r?r:(s->size!=sizeof(*s)||s->version!=SF_IPC_STATS_VERSION?SF_BAD_FORMAT:0);}
static void ipc_stats_line(void){
 SfIpcStats s;int r=ipc_stats_read(&s);if(r){sf_log("ipc-stats: failed code=");sf_number(-r);sf_log("\n");return;}
 sf_log("ipc-stats: send-bytes=");sf_number(s.send_bytes);sf_log(" receive-bytes=");sf_number(s.receive_bytes);sf_log("\n");
}
static int ipc_copy_increased(uint64_t before,uint64_t after){return before>UINT64_MAX-sizeof(SfMessage)?after==UINT64_MAX:after>=before+sizeof(SfMessage);}
static void ipc_stats_test(void){
 SfIpcStats before,after;memset(&before,0xa5,sizeof(before));
 sf_assert(sf_sys(SYS_IPC_STATS,(uint64_t)&before,sizeof(before)-1,0)==SF_INVALID&&before.size==0xa5a5a5a5u,"IPC counters reject size without touching output");
 sf_assert(sf_sys(SYS_IPC_STATS,(uint64_t)&before,sizeof(before),1)==SF_INVALID,"IPC counters reject reserved field");
 sf_assert(sf_ipc_stats((void*)SF_BUNDLE)==SF_DENIED,"IPC counters refuse read-only output buffer");
 int ep=sf_sys(SYS_EP_CREATE,0,0,0);sf_assert(ep>0&&!ipc_stats_read(&before),"root reads IPC counters without extra endpoint allocation");
 SfMessage m;sf_message(&m,OP_ECHO,0xc0b1);sf_assert(!sf_send(ep,&m)&&sf_recv(ep,&m,sf_deadline(100))==1,"private IPC round trip for copy observation");
 sf_assert(!ipc_stats_read(&after)&&ipc_copy_increased(before.send_bytes,after.send_bytes)&&ipc_copy_increased(before.receive_bytes,after.receive_bytes),"actual IPC user copies increase counters");
 sf_assert(!sf_sys(SYS_CAP_CLOSE,ep,0,0),"IPC counter diagnostic releases private endpoint");
 sf_log("IPC COPY TEST PASSED\n");
}

static void page_limit_line(char*line){
 char*words[2];int count=sf_arguments_split(line,words,2);uint32_t pid=0,pages=0;long r=SF_INVALID;
 if((count==1||count==2)&&decimal_u32(words[0],&pid)&&(count==1||decimal_u32(words[1],&pages)))
  r=sf_task_page_limit(pid,pages,count==1?SF_LIMIT_QUERY:SF_LIMIT_SET);
 if(r<0){sf_log("page-limit: failed code=");sf_number(-r);sf_log("\n");return;}
 long used=sf_task_page_usage(pid);if(used<0){sf_log("page-limit: failed code=");sf_number(-used);sf_log("\n");return;}
 sf_log("page-limit: pid=");sf_number(pid);sf_log(" limit=");sf_number(r);sf_log(" used=");sf_number(used);sf_log("\n");
}
static void page_quota_test(void){
 long root_used=sf_task_page_usage(1);sf_assert(root_used>0,"root pool-page ownership is counted");
 memset(staging,0,4096);staging[0]=0xeb;staging[1]=0xfe;
 SfLoadPlan p={0};p.size=sizeof(p);p.count=1;p.image_base=SF_BASE;p.segments[0]=(SfLoadSegment){0,4096,(uint64_t)staging,5,0};
 for(unsigned i=0;i<32;i++)sf_assert(sf_spawn_limited(&p,1)==SF_NO_SPACE&&sf_task_page_usage(1)==root_used,"tiny construction budgets roll back without charging parent");
 int pid=sf_spawn_limited(&p,64);sf_assert(pid>1,"explicit page budget admits isolated loop");
 long used=sf_task_page_usage(pid);sf_assert(used>0&&used<=64&&sf_task_page_limit(pid,0,SF_LIMIT_QUERY)==64,"page tables and private image fit configured budget");
 sf_assert(sf_task_page_limit(pid,used-1,SF_LIMIT_SET)==SF_BUSY&&sf_task_page_limit(pid,0,SF_LIMIT_QUERY)==64,"below-used page lowering is busy with no mutation");
 sf_assert(sf_task_page_limit(pid,used,SF_LIMIT_SET)==used&&sf_task_page_usage(pid)==used,"exact-used lowering preserves private mappings");
 uint64_t before=sf_ticks();while(sf_ticks()-before<3)sf_yield();
 sf_assert(sf_sys(SYS_TASK_STATUS,pid,0,0)==SF_TASK_RUNNING,"budgeted noncooperating task remains isolated and preemptible");
 sf_assert(!sf_sys(SYS_KILL,pid,0,0),"page quota diagnostic kills owned loop");
 sf_assert(sf_task_page_usage(pid)==SF_NOT_FOUND&&sf_task_page_limit(pid,0,SF_LIMIT_QUERY)==SF_NOT_FOUND&&sf_task_page_usage(1)==root_used,"dead owner reclaimed without changing parent usage");
 pid=sf_sys(SYS_SPAWN,(uint64_t)&p,0,0);sf_assert(pid>1&&sf_task_page_limit(pid,0,SF_LIMIT_QUERY)==SF_PAGE_LIMIT_MAX,"ordinary spawn on reused slot defaults independently");
 sf_assert(!sf_sys(SYS_KILL,pid,0,0)&&sf_task_page_usage(1)==root_used,"default child cleanup preserves parent accounting");
 sf_log("PAGE QUOTA TEST PASSED\n");
}

static void cap_limit_line(char*line){
 char*words[2];int count=sf_arguments_split(line,words,2);uint32_t pid=0,budget=0;long r=SF_INVALID;
 if((count==1||count==2)&&decimal_u32(words[0],&pid)&&(count==1||decimal_u32(words[1],&budget)))
  r=sf_sys(SYS_TASK_CAP_LIMIT,pid,budget,count==1?SF_LIMIT_QUERY:SF_LIMIT_SET);
 if(r<0){sf_log("cap-limit: failed code=");sf_number(-r);sf_log("\n");return;}
 long used=sf_task_cap_usage(pid);if(used<0){sf_log("cap-limit: failed code=");sf_number(-used);sf_log("\n");return;}
 sf_log("cap-limit: pid=");sf_number(pid);sf_log(" limit=");sf_number(r);sf_log(" used=");sf_number(used);sf_log("\n");
}
static void child_test_signal(uint32_t pid,uint64_t stage){Child*c=child(pid);SfMessage m;sf_message(&m,OP_ECHO,stage);sf_assert(c&&!sf_send(c->reply,&m),"diagnostic signals owned child");}
static void cap_limit_wait(uint32_t pid,uint64_t stage){
 SfMessage m;long from=sf_recv(fs_ep,&m,sf_deadline(3000));sf_assert(from==(long)pid&&m.h.flags==pid&&m.h.opcode==OP_HELLO&&m.h.request_id==stage&&!m.status,"cap limit test authenticates child stage");
}
static void cap_limit_test(void){
 int pid=launch_app_root("caplimit.sfk","/tmp");sf_assert(pid>0,"cap limit test owns waiting application");
 sf_assert(sf_task_cap_usage(pid)==2&&sf_sys(SYS_TASK_CAP_LIMIT,pid,0,SF_LIMIT_QUERY)==32&&sf_sys(SYS_TASK_CAP_LIMIT,pid,3,SF_LIMIT_SET)==3,"root observes inherited handles and configures budget before release");
 child_test_signal(pid,1);cap_limit_wait(pid,1);
 sf_assert(sf_task_cap_usage(pid)==3&&sf_sys(SYS_TASK_CAP_LIMIT,pid,2,SF_LIMIT_SET)==SF_BUSY&&sf_sys(SYS_TASK_CAP_LIMIT,pid,0,SF_LIMIT_QUERY)==3,"root observes full usage and busy lowering preserves budget");
 int ep=sf_sys(SYS_EP_CREATE,0,0,0);sf_assert(ep>0&&!sf_sys(SYS_CAP_CLOSE,ep,0,0),"refused child allocations leave endpoint pool available");
 child_test_signal(pid,2);cap_limit_wait(pid,2);
 sf_assert(sf_task_cap_usage(pid)==2&&sf_sys(SYS_TASK_CAP_LIMIT,pid,2,SF_LIMIT_SET)==2,"root observes released reference before lowering budget");
 child_test_signal(pid,3);sf_assert(!wait_app(pid),"inherited two-cap file transport stays usable at budget");
 sf_assert(sf_task_cap_usage(pid)==SF_NOT_FOUND&&sf_sys(SYS_TASK_CAP_LIMIT,pid,0,SF_LIMIT_QUERY)==SF_NOT_FOUND,"retired child usage/limit is not configurable");
 sf_log("CAP LIMIT TEST PASSED\n");
}
static void ipc_cancel_test(void){
 int pid=launch_app("cancelprobe.sfk");sf_assert(pid>0,"send withdrawal test owns gated application");
 SfMessage keep;sf_message(&keep,OP_HELLO,0x100000007ull);keep.status=0xca11;
 sf_assert(!sf_send(fs_ep,&keep),"file service queues same ID under the manager's own identity");child_test_signal(pid,0xcace);
 uint64_t end=sf_ticks()+500;long status;
 /* Do not dequeue manager requests: the application tests a still-queued send. */
 while((status=sf_sys(SYS_TASK_STATUS,pid,0,0))==SF_TASK_RUNNING&&sf_ticks()<end)sf_yield();
 if(status==SF_TASK_RUNNING){sf_sys(SYS_KILL,pid,SF_TIMEOUT,0);app_reap(pid);sf_assert(0,"send withdrawal probe timed out");return;}
 sf_assert(!status,"send withdrawal application completes");app_reap(pid);
 SfMessage m;long from=sf_recv(fs_ep,&m,sf_deadline(1000));
 sf_assert(from==1&&m.h.flags==1&&m.h.request_id==keep.h.request_id&&m.status==keep.status,"withdrawal preserves another sender's same-ID request");
 sf_assert(sf_recv(fs_ep,&m,sf_deadline(20))==SF_TIMEOUT,"withdrawn application request is absent");
 sf_log("IPC CANCEL TEST PASSED\n");
}
/* 生命周期 RPC 走独立的管理端点：应用没有该端点，回复也落在独立端点。
 * 这里用根身份自调用，验证真实的 IPC、身份检查和状态机返回值。 */
static void manager_serve(void){
 SfMessage m;long from=sf_recv(manager_ep,&m,sf_deadline(0));if(from<0)return;
 if(sf_lifecycle_dispatch(&manager,(uint32_t)from,1,&m))m.status=SF_INVALID;
 sf_send(manager_reply,&m);
}
static void lifecycle_rpc_test(void){
 SfMessage m;sf_message(&m,OP_MODULE_STATUS,++requests);m.mode=echo_slot;sf_assert(!sf_send(manager_ep,&m),"lifecycle rpc queues root request");manager_serve();
 long from=sf_recv(manager_reply,&m,sf_deadline(100));sf_assert(from==1&&m.h.request_id==requests&&!m.status&&m.mode==MOD_ACTIVE&&m.length&&!strcmp((char*)m.data,"org.spiritfox.echo"),"lifecycle status rpc reports active echo");
 sf_message(&m,OP_MODULE_LIST,++requests);m.length=16;sf_assert(!sf_send(manager_ep,&m),"lifecycle list request accepted");manager_serve();
 from=sf_recv(manager_reply,&m,sf_deadline(100));sf_assert(from==1&&!m.status&&m.length==16&&m.data[echo_slot]==MOD_ACTIVE,"lifecycle list rpc reports slot states");
 sf_message(&m,OP_MODULE_START,++requests);m.mode=echo_slot;sf_assert(!sf_send(manager_ep,&m),"lifecycle start request accepted");manager_serve();
 from=sf_recv(manager_reply,&m,sf_deadline(100));sf_assert(from==1&&m.status==SF_INVALID,"lifecycle start rpc rejects active module without mutating state");
 sf_message(&m,OP_MODULE_STATUS,++requests);m.mode=99;sf_assert(!sf_send(manager_ep,&m),"lifecycle out-of-range request accepted");manager_serve();
 from=sf_recv(manager_reply,&m,sf_deadline(100));sf_assert(from==1&&m.status==SF_NOT_FOUND,"lifecycle rpc rejects out-of-range slot");
 sf_log("LIFECYCLE RPC TEST PASSED\n");
}
static void trust_show(void){
 if(sf_trust_validate(&sf_trust_store)){sf_log("trust: INVALID\n");return;}
 sf_log("trust: build-time keys=");sf_number(sf_trust_store.count);sf_log("\n");
 for(unsigned i=0;i<sf_trust_store.count;i++){const SfTrustKey*k=&sf_trust_store.keys[i];uint8_t hash[32];char id[65];sf_sha256(hash,k->public_key,32);
  for(unsigned j=0;j<32;j++){id[j*2]="0123456789abcdef"[hash[j]>>4];id[j*2+1]="0123456789abcdef"[hash[j]&15];}id[64]=0;
  sf_log("key ");sf_log(id);sf_log(k->revoked?" REVOKED":" ACTIVE");if(k->usage&SF_TRUST_MODULE)sf_log(" module");if(k->usage&SF_TRUST_APP)sf_log(" app");sf_log("\n");
 }
}
static void threads_test(void){
 int pid=launch_app("threadprobe.sfk");sf_assert(pid>0,"thread probe application launched");
 sf_assert(sf_thread_limit(pid,0,SF_LIMIT_USED)>=1,"root observes live thread usage");
 sf_assert(!wait_app(pid),"thread probe creates, limits and reuses thread slots");
 sf_assert(sf_thread_limit(pid,0,SF_LIMIT_USED)==SF_NOT_FOUND,"retired thread budget is not configurable");
 sf_log("THREAD TEST PASSED\n");
}
static void threads_line(char*line){
 char*words[2];int count=sf_arguments_split(line,words,2);uint32_t pid=0,limit=0;long r=SF_INVALID;
 if((count==1||count==2)&&decimal_u32(words[0],&pid)&&(count==1||decimal_u32(words[1],&limit)))
  r=sf_thread_limit(pid,limit,count==1?SF_LIMIT_QUERY:SF_LIMIT_SET);
 if(r<0){sf_log("threads: failed code=");sf_number(-r);sf_log("\n");return;}
 long used=sf_thread_limit(pid,0,SF_LIMIT_USED);if(used<0){sf_log("threads: failed code=");sf_number(-used);sf_log("\n");return;}
 sf_log("threads: pid=");sf_number(pid);sf_log(" limit=");sf_number(r);sf_log(" used=");sf_number(used);sf_log("\n");
}
static void cpu_budget_line(char*line){
 char*words[2];int count=sf_arguments_split(line,words,2);uint32_t pid=0,budget=0;long r=SF_INVALID;
 if((count==1||count==2)&&decimal_u32(words[0],&pid)&&(count==1||decimal_u32(words[1],&budget)))
  r=sf_task_cpu_budget(pid,budget,count==1?SF_LIMIT_QUERY:SF_LIMIT_SET);
 if(r<0){sf_log("cpu-budget: failed code=");sf_number(-r);sf_log("\n");return;}
 long used=sf_task_cpu_budget(pid,0,SF_LIMIT_USED);if(used<0){sf_log("cpu-budget: failed code=");sf_number(-used);sf_log("\n");return;}
 sf_log("cpu-budget: pid=");sf_number(pid);sf_log(" limit=");sf_number(r);sf_log(" used=");sf_number(used);sf_log("\n");
}
static void prompt(void){
 const SfSession*s=sf_session_current(&sessions);
 if(!s)sf_log("login> ");
 else if(!s->uid)sf_log("fox> ");
 else{sf_log(s->name);sf_log("@fox> ");}
}
static void help_text(void){
 sf_log("help | login <user> [password] | logout | whoami | users | sessions | ls [path] | cat <path> | write <path> <text> | truncate <path> <bytes> | stat <path> | rm <path> | mkdir <path> | rmdir <path>\n");
 sf_log("run <app or installed> [args] | runat <read-only-dir> <app> [args]\nrunenv [NAME=VALUE ...] -- <app> [args] | runenvat <dir> [NAME=VALUE ...] -- <app> [args]\n");
 sf_log("Administrator only: priority <pid> [0..3] | cap-limit <pid> [0..32] | page-limit <pid> [0..4096] | threads <pid> [1..8] | cpu-budget <pid> [0..100] | ipc-stats-test | trust | revoke-key /disk/pkg\n");
 sf_log("install /disk/bin/app.sfk | persist | stop echo | start echo | echo-status | echo-crash | echo-hang | echo-bad-reply | echo-bad-op | mount /disk | unmount /disk | stop disk | start disk | selftest | shutdown\n");
 sf_log("ifconfig | dhcp | ping <ip> | udp <ip> <port> <text> | tcp <ip> <port> <text> | open <ip> <port> | send <handle> <text> | recv <handle> | close <handle> | conns | stop native | start native | native-status | native-crash | native-hang | native-bad-reply | native-bad-op\n");
}
static int root_only(const char*line){
 static const char*const names[]={"persist","dma-test ","disk-crash","install ","trust","priority-test","priority ","ipc-stats-test","threads-test","threads ","page-quota-test","cap-limit-test","ipc-cancel-test","page-limit ","cap-limit ","cpu-budget ","cpu-state-test","revoke-key ","channel-test","echo-channel-test","echo-capacity-test","native-capacity-test","native-drain-test","native-crash","native-hang","native-bad-reply","native-bad-op","start native","stop native","echo-crash","echo-hang","echo-bad-reply","echo-bad-op","start echo","stop echo","mount /disk","unmount /disk","start disk","stop disk","dhcp","selftest","shutdown"};
 for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++){size_t len=strlen(names[i]);if(!strcmp(line,names[i])||(names[i][len-1]==' '&&!strncmp(line,names[i],len)))return 1;}
 return 0;
}
static void login_attempt(const char*name,const char*password){
 uint32_t id=0;int r=sf_session_login(&sessions,&accounts,name,password,&id);
 if(r==SF_NO_SPACE){sf_log("login: no free session slot\n");return;}
 if(r){sf_log("login: authentication failed\n");return;}
 const SfSession*s=sf_session_current(&sessions);
 sf_log("login: ");sf_log(s->name);sf_log(" uid=");sf_number(s->uid);sf_log(" gid=");sf_number(s->gid);sf_log(" session=");sf_number(id);sf_log(" home=");sf_log(s->home);sf_log("\n");
}
static void login_line(char*args){
 if(!args||!*args){sf_log("login: expected user name\n");return;}
 char*name=args,*p=name;while(*p&&*p!=' ')p++;size_t len=(size_t)(p-name);
 if(len>SF_ACCOUNT_NAME_MAX){sf_log("login: invalid user name\n");return;}
 if(*p){*p=0;char*secret=p+1;while(*secret==' ')secret++;if(*secret){login_attempt(name,secret);return;}}
 memcpy(password_user,name,len+1);password_used=0;memset(password_buffer,0,sizeof(password_buffer));password_pending=1;sf_log("password: ");
}
static void logout_line(void){
 uint32_t ended=0;int r=sf_session_logout(&sessions,&ended);
 if(r){sf_log("logout: no active session\n");return;}
 sf_ns_reap(&ns,sf_session_owner(ended));
 sf_log("logout: session ");sf_number(ended);sf_log(" closed\n");
}
static void whoami_line(void){
 const SfSession*s=sf_session_current(&sessions);if(!s){sf_log("whoami: no active session\n");return;}
 sf_log("whoami: name=");sf_log(s->name);sf_log(" uid=");sf_number(s->uid);sf_log(" gid=");sf_number(s->gid);
 sf_log(" session=");sf_number(sf_session_active(&sessions));sf_log(" home=");sf_log(s->home);sf_log("\n");
}
static void users_line(void){
 for(unsigned i=0;i<accounts.count;i++){char home[SF_ACCOUNT_HOME_MAX+1];const SfAccount*a=&accounts.accounts[i];
  if(sf_account_home(a,home,sizeof(home)))continue;
  sf_log("user: ");sf_log(a->name);sf_log(" uid=");sf_number(a->uid);sf_log(" gid=");sf_number(a->gid);sf_log(" home=");sf_log(home);sf_log("\n");
 }
}
static void sessions_line(void){
 sf_log("sessions: ");sf_number(sf_session_count(&sessions));sf_log(" active=");sf_number(sf_session_active(&sessions));sf_log("\n");
 for(unsigned i=0;i<SF_SESSION_MAX;i++){const SfSession*s=&sessions.slots[i];if(!s->used)continue;
  sf_log("session: id=");sf_number(i+1);sf_log(" user=");sf_log(s->name);sf_log(" uid=");sf_number(s->uid);
  sf_log(sf_session_active(&sessions)==i+1?" active=1\n":" active=0\n");
 }
}
static void command(char*line){
 if(!*line)return;
 if(!strcmp(line,"help")){help_text();return;}
 if(!strcmp(line,"login")||!memcmp(line,"login ",6)){login_line(line[5]?line+6:0);return;}
 if(!strcmp(line,"logout")){logout_line();return;}
 if(!strcmp(line,"whoami")){whoami_line();return;}
 if(!strcmp(line,"users")){users_line();return;}
 if(!strcmp(line,"sessions")){sessions_line();return;}
 if(!sf_session_current(&sessions)){sf_log("login required; type login <user>\n");return;}
 if(root_only(line)&&!sf_session_is_root(&sessions)){sf_log("denied: root session required\n");return;}
 if(!strcmp(line,"rm")||!memcmp(line,"rm ",3))namespace_mutate_line(line+2,0);
 else if(!strcmp(line,"mkdir")||!memcmp(line,"mkdir ",6))namespace_mutate_line(line+5,1);
 else if(!strcmp(line,"rmdir")||!memcmp(line,"rmdir ",6))namespace_mutate_line(line+5,2);
 else if(!strcmp(line,"stat")||!memcmp(line,"stat ",5))namespace_stat_line(line+4);
 else if(!strcmp(line,"persist"))sf_persist_status();
 else if(!strcmp(line,"dma-test write"))sf_storage_dma_probe(0);
 else if(!strcmp(line,"dma-test read"))sf_storage_dma_probe(1);
 else if(!strcmp(line,"disk-crash")){if(sf_storage_crash_test())sf_log("disk: crash test refused\n");}
 else if(!memcmp(line,"install ",8)){int r=sf_persist_install(line+8);if(!r)sf_log("install: COMMITTED\n");else if(r==1)sf_log("install: UNCHANGED\n");else{sf_log("install: failed code=");sf_number(-r);sf_log("\n");}}
 else if(!strcmp(line,"trust")){trust_show();sf_persist_trust_status();}
 else if(!strcmp(line,"priority-test"))priority_test();
 else if(!strcmp(line,"priority")||!memcmp(line,"priority ",9))priority_line(line+8);
 else if(!strcmp(line,"ipc-stats"))ipc_stats_line();
 else if(!strcmp(line,"ipc-stats-test"))ipc_stats_test();
 else if(!strcmp(line,"threads-test"))threads_test();
 else if(!strcmp(line,"threads")||!memcmp(line,"threads ",8))threads_line(line+7);
 else if(!strcmp(line,"cpu-budget")||!memcmp(line,"cpu-budget ",11))cpu_budget_line(line+10);
 else if(!strcmp(line,"page-quota-test"))page_quota_test();
 else if(!strcmp(line,"cap-limit-test"))cap_limit_test();
 else if(!strcmp(line,"ipc-cancel-test"))ipc_cancel_test();
 else if(!strcmp(line,"page-limit")||!memcmp(line,"page-limit ",11))page_limit_line(line+10);
 else if(!strcmp(line,"cap-limit")||!memcmp(line,"cap-limit ",10))cap_limit_line(line+9);
 else if(!memcmp(line,"revoke-key ",11)){int r=sf_persist_revoke(line+11);if(!r)sf_log("revoke: OK\n");else if(r==1)sf_log("revoke: UNCHANGED\n");else{sf_log("revoke: failed code=");sf_number(-r);sf_log("\n");}}
 else if(!strcmp(line,"modules")){sf_log("sf-init ACTIVE pinned; sf-native ");sf_log(manager.modules[runtime_slot].state==MOD_ACTIVE?"ACTIVE":"STOPPED");sf_log("; echo ");sf_log(manager.modules[echo_slot].state==MOD_ACTIVE?"ACTIVE\n":"STOPPED\n");}
 else if(!strcmp(line,"echo")){sf_log(echo_call(echo_ep,echo_pid)==0?"echo: ping\n":"echo: service unavailable\n");}
 else if(!strcmp(line,"ifconfig"))net_status();
 else if(!strcmp(line,"dhcp"))net_dhcp_line();
 else if(!memcmp(line,"ping ",5))net_ping_line(line+5);
 else if(!memcmp(line,"udp ",4))net_packet_line(line+4,OP_NET_UDP,"udp");
 else if(!memcmp(line,"tcp ",4))net_packet_line(line+4,OP_NET_TCP,"tcp");
 else if(!memcmp(line,"open ",5))net_conn_line(line+5,OP_NET_OPEN);
 else if(!memcmp(line,"send ",5))net_conn_line(line+5,OP_NET_SEND);
 else if(!memcmp(line,"recv ",5))net_conn_line(line+5,OP_NET_RECV);
 else if(!memcmp(line,"close ",6))net_conn_line(line+6,OP_NET_CLOSE);
 else if(!strcmp(line,"conns"))net_conns_line();
 else if(!memcmp(line,"run ",4))run_line(line+4,0);
 else if(!memcmp(line,"runat ",6))run_line(line+6,1);
 else if(!memcmp(line,"runenv ",7))run_environment_line(line+7,0);
 else if(!memcmp(line,"runenvat ",9))run_environment_line(line+9,1);
 else if(!memcmp(line,"truncate ",9)){char*path=line+9,*text=path;while(*text&&*text!=' ')text++;uint64_t bytes=0;int r=SF_INVALID;uint32_t uid,gid,owner;
  if(session_identity(&uid,&gid,&owner))r=SF_DENIED;
  else if(*text){*text++=0;r=parse_size(text,&bytes);if(!r){int h=shell_open(path,OPEN_WRITE);r=h<0?h:sf_ns_truncate(&ns,owner,h,bytes);if(h>=0)sf_ns_close(&ns,owner,h);}}
  if(!r)sf_log("truncate: OK\n");else{sf_log("truncate: failed code=");sf_number(-r);sf_log("\n");}}
 else if(!strcmp(line,"unmount /disk")){int r=sf_storage_unmount();sf_log(!r?"disk: UNMOUNTED\n":"disk: unmount refused\n");}
 else if(!strcmp(line,"mount /disk")){int r=sf_storage_mount();if(!r&&sf_storage_writable()){sf_persist_attach();r=sf_persist_mount(&ns);}sf_log(!r?"disk: MOUNTED\n":"disk: mount refused\n");}
 else if(!strcmp(line,"stop disk")){int r=sf_storage_stop();sf_log(!r?"disk: STOPPED\n":"disk: stop refused\n");}
 else if(!strcmp(line,"start disk")){int r=sf_storage_start();if(!r)sf_persist_attach();sf_log(!r?"disk: ACTIVE\n":"disk: start refused\n");}
 else if(!strcmp(line,"channel-test")){channel_test(0);}
 else if(!strcmp(line,"echo-channel-test")){channel_test(1);}
 else if(!strcmp(line,"echo-capacity-test")){echo_capacity_test();}
 else if(!strcmp(line,"native-status"))native_status();
 else if(!strcmp(line,"native-capacity-test"))native_capacity_test();
 else if(!strcmp(line,"native-drain-test"))native_drain_test();
 else if(!strcmp(line,"cpu-state-test")){
  int a=launch_app("fpprobe.sfk"),b=launch_app("fpprobe.sfk");
  sf_assert(a>0&&b>0&&sf_sys(SYS_TASK_STATUS,a,0,0)==SF_TASK_RUNNING&&sf_sys(SYS_TASK_STATUS,b,0,0)==SF_TASK_RUNNING,"two CPU state probes overlap");
  int ar=wait_app(a),br=wait_app(b);sf_assert(!ar&&!br,"interleaved private x87/MMX/SSE contexts");sf_log("CPU STATE PAIR PASSED\n");
 }
 else if(!strcmp(line,"native-crash")){int r=native_watch.state==SF_SUP_RUNNING&&runtime_pid?sf_sys(SYS_KILL,runtime_pid,SF_DEVICE_GONE,0):SF_STALE_HANDLE;sf_log(!r?"native: crash injected\n":"native: crash refused\n");}
 else if(!strcmp(line,"native-hang")||!strcmp(line,"native-bad-reply")||!strcmp(line,"native-bad-op")){
  unsigned mode=!strcmp(line,"native-hang")?1:!strcmp(line,"native-bad-reply")?2:3;SfMessage m;sf_message(&m,OP_NATIVE_DIAGNOSTIC,++requests);m.mode=mode;
  int r=native_watch.state==SF_SUP_RUNNING?sf_send(runtime_ep,&m):SF_STALE_HANDLE;sf_log(r?"native: injection refused\n":mode==1?"native: hang injected\n":mode==2?"native: bad reply injected\n":"native: bad opcode injected\n");
 }
 else if(!strcmp(line,"stop native")){int r=native_stop_service();if(!r)sf_supervisor_disable(&native_watch);sf_log(!r?"native: STOPPED\n":"native: BUSY\n");}
 else if(!strcmp(line,"start native")){int r=native_start_service();if(!r){sf_supervisor_enable(&native_watch,sf_ticks(),1);native_health_at=echo_after(sf_ticks(),100);}else if(r!=SF_INVALID)sf_supervisor_disable(&native_watch);sf_log(!r?"native: ACTIVE\n":manager.modules[runtime_slot].state==MOD_DRAINING?"native: draining\n":r==SF_INVALID?"native: already active\n":"native: spawn failed\n");}
 else if(!strcmp(line,"echo-status"))echo_status();
 else if(!strcmp(line,"echo-hang")||!strcmp(line,"echo-bad-reply")||!strcmp(line,"echo-bad-op")){
  unsigned mode=!strcmp(line,"echo-hang")?1:!strcmp(line,"echo-bad-reply")?2:3;SfMessage m;sf_message(&m,OP_ECHO_DIAGNOSTIC,++requests);m.mode=mode;
  int r=echo_watch.state==SF_SUP_RUNNING?sf_send(echo_ep,&m):SF_STALE_HANDLE;
  sf_log(r?"echo: injection refused\n":mode==1?"echo: hang injected\n":mode==2?"echo: bad reply injected\n":"echo: bad opcode injected\n");
 }
 else if(!strcmp(line,"echo-crash")){int r=echo_pid&&echo_peer(echo_pid)?sf_sys(SYS_KILL,echo_pid,SF_DEVICE_GONE,0):SF_STALE_HANDLE;sf_log(!r?"echo: crash injected\n":"echo: crash refused\n");}
 else if(!strcmp(line,"stop echo")){int r=manager.modules[echo_slot].state==MOD_STOPPED?0:echo_stop_service();if(!r)sf_supervisor_disable(&echo_watch);sf_log(!r?"echo: STOPPED\n":"echo: BUSY\n");}
 else if(!strcmp(line,"start echo")){int r=echo_start_service();if(!r){uint64_t now=sf_ticks();sf_supervisor_enable(&echo_watch,now,1);echo_health_at=echo_after(now,100);}else if(r!=SF_INVALID)sf_supervisor_disable(&echo_watch);sf_log(!r?"echo: ACTIVE\n":r==SF_INVALID?"echo: already active\n":"echo: spawn failed\n");}
 else if(!strcmp(line,"ls")||!memcmp(line,"ls ",3)){const char*p=line[2]?line+3:"/";uint32_t uid,gid,owner;int h=session_identity(&uid,&gid,&owner)?SF_DENIED:shell_open(p,1);char out[256];long n=h<0?h:sf_ns_enumerate(&ns,owner,h,out,sizeof(out));if(n>=0)console_write(out,n);else sf_log("ls: error\n");if(h>=0)sf_ns_close(&ns,owner,h);}
 else if(!memcmp(line,"cat ",4)){uint32_t uid,gid,owner;int h=session_identity(&uid,&gid,&owner)?SF_DENIED:shell_open(line+4,1);char out[256];
  if(h<0)sf_log("cat: error\n");else{for(unsigned page=0;page<16;page++){long n=sf_ns_read(&ns,owner,h,out,sizeof(out));if(n<0){sf_log("cat: error\n");break;}if(!n)break;console_write(out,n);}sf_ns_close(&ns,owner,h);}}
 else if(!memcmp(line,"write ",6)){char*p=line+6,*text=p;while(*text&&*text!=' ')text++;uint32_t uid,gid,owner;int ir=session_identity(&uid,&gid,&owner);
  if(ir)sf_log("write: error\n");else if(*text){*text++=0;int h=shell_open(p,2);if(h==SF_NOT_FOUND){sf_ns_create_owned(&ns,p,NODE_FILE,0,uid,gid,0644);h=shell_open(p,2);}long n=h<0?h:sf_ns_write(&ns,owner,h,text,strlen(text));sf_log(n>=0?"write: OK\n":"write: error\n");if(h>=0)sf_ns_close(&ns,owner,h);}else sf_log("write: expected path and text\n");}
 else if(!strcmp(line,"selftest")){object_tests();sf_assert(!echo_call(echo_ep,echo_pid),"shell echo health");sf_log("SELFTEST OK\n");}
 else if(!strcmp(line,"shutdown")){sf_log("SHUTDOWN OK\n");sf_sys(SYS_SHUTDOWN,0,0,0);}
 else if(*line)sf_log("unknown command; type help\n");
}
long sf_main(const SfBootArgs*a,size_t size){
 boot=a;if(size!=sizeof(*a)||a->abi_major!=1||!a->bundle)return SF_INVALID;
 if(sf_trust_init(&sf_trust,&sf_trust_store))return SF_INVALID;
 sf_log("[INIT] sf-init.sfmod in ring 3; services live outside Core 0\n");manager_ep=sf_sys(SYS_EP_CREATE,0,0,0);fs_ep=sf_sys(SYS_EP_CREATE,0,0,0);manager_reply=sf_sys(SYS_EP_CREATE,0,0,0);sf_assert(manager_ep>0&&fs_ep>0&&manager_reply>0,"manager, file service and lifecycle reply channels allocated");
 int init=sfmod_add(&manager,"org.spiritfox.init",0,1);sfmod_start(&manager,init);runtime_slot=sfmod_add(&manager,"org.spiritfox.native",0,0);
 echo_slot=sfmod_add(&manager,"org.spiritfox.echo",0,0);sfmod_start(&manager,echo_slot);echo_ep=sf_sys(SYS_EP_CREATE,0,0,0);echo_pid=spawn_echo("echo.sfmod",echo_ep,RIGHT_RECV);sf_assert((int)echo_pid>0,"verified echo.sfmod staged in private address space");
 lifecycle_rpc_test();
 lifecycle_tests();namespace_setup();identity_setup();object_tests();sf_assert(!native_start_service(),"native runtime module active");sf_assert((int)runtime_reply>0,"runtime replies have an isolated endpoint");runtime_tests();
 sf_storage_init(&manager,&ns,spawn_storage_package);int storage=sf_storage_start();if(!storage)storage=sf_storage_mount();
 if(storage){sf_log("STORAGE UNAVAILABLE code=");sf_number(-storage);sf_log("\n");}
 else{sf_storage_tests();if(sf_storage_writable()){int r=sf_persist_start(spawn_storage_package);if(r){sf_log("PERSIST SPAWN FAILED code=");sf_number(-r);sf_log("\n");}r=sf_persist_mount(&ns);if(r){sf_log("PERSIST UNAVAILABLE code=");sf_number(-r);sf_log("\n");}else sf_storage_write_tests();}int ep=sf_sys(SYS_EP_CREATE,0,0,0);int disk_module=spawn_echo("/disk/modules/disk-echo.sfmod",ep,RIGHT_RECV);
  if(disk_module>0){int slot=sfmod_add(&manager,"org.spiritfox.disk-echo",0,0);sf_assert(slot>=0&&!sfmod_start(&manager,slot)&&!echo_call(ep,disk_module),"signed sfmod loaded from disk without bundle fallback");sfmod_prepare(&manager,slot);sfmod_stop(&manager,slot);sf_sys(SYS_KILL,disk_module,0,0);echo_forget(disk_module);sfmod_unload(&manager,slot);}else{sf_log("DISK PACKAGE REJECTED module code=");sf_number(-disk_module);sf_log("\n");}sf_sys(SYS_CAP_REVOKE,ep,0,0);
  int app=launch_app("/disk/bin/diskhello.sfk");if(app>0)sf_assert(wait_app(app)==0,"signed disk application runs through native runtime");else{sf_log("DISK PACKAGE REJECTED app code=");sf_number(-app);sf_log("\n");}}
 {int net=net_start();if(!net)sf_log("net: READY\n");else if(net!=SF_NOT_FOUND){sf_log("NET UNAVAILABLE code=");sf_number(-net);sf_log("\n");}}
 sf_assert(!sf_supervisor_init(&echo_watch,3,25,1000),"echo restart policy configured");sf_supervisor_enable(&echo_watch,sf_ticks(),1);echo_health_at=echo_after(sf_ticks(),100);
 sf_assert(!sf_supervisor_init(&native_watch,3,25,1000),"native restart policy configured");sf_supervisor_enable(&native_watch,sf_ticks(),1);native_health_at=echo_after(sf_ticks(),100);
 sf_log("BOOT TESTS PASSED\nSpiritFoxOS ready. Type help.\n");prompt();char line[192]={0};unsigned used=0;
 for(;;){echo_supervise();native_supervise();manager_serve();long c=sf_sys(SYS_CONSOLE_READ,0,0,0);
  if(c>=0){
   if(password_pending){
    if(c=='\r'||c=='\n'){char secret[SF_ACCOUNT_PASSWORD_MAX+1];memcpy(secret,password_buffer,password_used);secret[password_used]=0;password_pending=0;memset(password_buffer,0,sizeof(password_buffer));sf_log("\n");login_attempt(password_user,secret);memset(secret,0,sizeof(secret));prompt();}
    else if(c==8||c==127){if(password_used)password_used--;}
    else if(c>=32&&c<127&&password_used<SF_ACCOUNT_PASSWORD_MAX)password_buffer[password_used++]=(char)c;
   }else if(c=='\r'||c=='\n'){sf_log("\n");line[used]=0;command(line);used=0;memset(line,0,sizeof(line));if(!password_pending)prompt();}
   else if(c==8||c==127){if(used){used--;sf_log("\b \b");}}
   else if(c>=32&&c<127&&used+1<sizeof(line)){line[used++]=c;char s[2]={(char)c,0};sf_log(s);}
  }else if(c==SF_EVENT_OVERFLOW){used=0;memset(line,0,sizeof(line));password_pending=0;memset(password_buffer,0,sizeof(password_buffer));sf_log("\nconsole input overflow; line discarded\n");prompt();}
  else{serve_once(10);sf_yield();}
 }
}
