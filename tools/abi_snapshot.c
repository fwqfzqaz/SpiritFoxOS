/* Compile the shared fixed-width layouts; never execute kernel/service code. */
#include <stdio.h>
#include "sf.h"
#include "net.h"
#include "package.h"
_Static_assert(sizeof(void*)==8&&sizeof(size_t)==8,"x86_64 snapshot only");
static int first=1;
static void key(const char *name){printf("%s\"%s\":",first?"":",",name);first=0;}
static void value(const char *name,uint64_t n){key(name);printf("%llu",(unsigned long long)n);}
static void status(const char *name,int64_t n){key(name);printf("%lld",(long long)n);}
#define U(n) value("constant."#n,(uint64_t)(n))
#define S(n) status("constant."#n,(int64_t)(n))
#define L(t) value("size."#t,sizeof(t));value("align."#t,_Alignof(t))
/* Type codes are fixed-width scalar categories, independent of host long. */
#define K(x) _Generic((x),uint8_t:1,int8_t:2,uint16_t:3,int16_t:4,uint32_t:5,int32_t:6,uint64_t:7,int64_t:8,default:0)
#define F(t,m) value("offset."#t"."#m,offsetof(t,m));value("width."#t"."#m,sizeof(((t*)0)->m));value("kind."#t"."#m,K(((t*)0)->m))
int main(void){
 printf("{\"format\":\"spiritfoxos-x86_64-abi-snapshot-v1\",\"values\":{");
 const uint32_t endian=1;
 value("model.pointer_bits",sizeof(void*)*8);value("model.little_endian",*(const uint8_t*)&endian);
 U(SF_ABI);U(SF_PAGE);U(SF_BASE);U(SF_IMAGE_MAX);U(SF_PIE_MIN);
 U(SF_NATIVE_STATIC);U(SF_NATIVE_RELATIVE);U(SF_RELOCATION_MAX);
 U(SF_STACK);U(SF_ARGS);U(SF_BUNDLE);U(SF_DMA);U(SF_DMA_BYTES);
 U(SF_MAX_TASKS);U(SF_MSG_BYTES);U(SF_ARGUMENT_MAX);U(SF_ENVIRONMENT_MAX);U(SF_ARGUMENT_BYTES);U(SF_TASK_RUNNING);
 U(SF_DEVICE_WRITE);U(SF_DEVICE_ISOLATED);U(SF_DEVICE_MODERN);U(SF_DEVICE_MMIO);
 U(SYS_EXIT);U(SYS_YIELD);U(SYS_LOG);U(SYS_TICKS);U(SYS_EP_CREATE);U(SYS_SEND);U(SYS_RECV);
 U(SYS_SPAWN);U(SYS_KILL);U(SYS_TASK_STATUS);U(SYS_CONSOLE_READ);U(SYS_SHUTDOWN);
 U(SYS_CAP_DERIVE);U(SYS_CAP_CLOSE);U(SYS_MAP_TEST);U(SYS_CAP_REVOKE);
 U(SYS_PCI_READ);U(SYS_PORT_IO);U(SYS_DEVICE_RELEASE);U(SYS_DMA_STATUS);
 U(SYS_TASK_PRIORITY);U(SF_PRIORITY_MIN);U(SF_PRIORITY_DEFAULT);U(SF_PRIORITY_MAX);
 U(SF_PRIORITY_AGING_TICKS);U(SF_PRIORITY_QUERY);U(SF_PRIORITY_SET);
 U(RIGHT_SEND);U(RIGHT_RECV);U(RIGHT_DELEGATE);
 S(SF_OK);S(SF_INVALID);S(SF_DENIED);S(SF_BAD_FORMAT);S(SF_BAD_SIGNATURE);S(SF_ABI_MISMATCH);
 S(SF_DEPENDENCY);S(SF_BUSY);S(SF_DRAINING);S(SF_TIMEOUT);S(SF_DEVICE_GONE);S(SF_NOT_SEEKABLE);
 S(SF_STALE_HANDLE);S(SF_REVOKED);S(SF_RESTART_REQUIRED);S(SF_WOULD_BLOCK);S(SF_NOT_FOUND);
 S(SF_READ_ONLY);S(SF_NO_SPACE);S(SF_EVENT_OVERFLOW);S(SF_COMMIT_UNCERTAIN);S(SF_VERSION_REJECTED);
 U(OP_ECHO);U(OP_ECHO_DIAGNOSTIC);U(OP_OPEN);U(OP_READ);U(OP_WRITE);U(OP_CLOSE);U(OP_SEEK);
 U(OP_POLL);U(OP_CREATE);U(OP_DESCRIBE);U(OP_ENUMERATE);U(OP_OPENAT);U(OP_ROOT);
 U(OP_TRUNCATE);U(OP_UNLINK);U(OP_STAT);U(OP_RMDIR);U(OP_DUP);U(OP_PREAD);U(OP_HELLO);
 U(OP_NATIVE_PREPARE);U(OP_NATIVE_STAGE);U(OP_NATIVE_HEALTH);U(OP_NATIVE_DIAGNOSTIC);
 U(OP_BLOCK_INFO);U(OP_BLOCK_READ);U(OP_BLOCK_WRITE);U(OP_BLOCK_STOP);U(OP_BLOCK_FLUSH);
 U(OP_BLOCK_WRITE_RANGE);U(OP_BLOCK_DMA_PROBE);U(OP_NET_INFO);U(OP_NET_PING);U(OP_NET_UDP);U(OP_NET_TCP);
 U(SF_FILE_READ_ONLY);U(SF_FILE_MOUNTED);U(SF_FILE_SEEKABLE);
 U(SF_NET_PAYLOAD_MAX);U(SF_PACKAGE_MAX);U(SF_TRUST_MAX);U(SF_TRUST_MODULE);U(SF_TRUST_APP);
 L(SfIpcHeader);F(SfIpcHeader,size);F(SfIpcHeader,protocol_major);F(SfIpcHeader,protocol_minor);
 F(SfIpcHeader,opcode);F(SfIpcHeader,flags);F(SfIpcHeader,request_id);
 F(SfIpcHeader,payload_bytes);F(SfIpcHeader,cap_count);F(SfIpcHeader,deadline_ns);
 L(SfMessage);F(SfMessage,h);F(SfMessage,status);F(SfMessage,handle);F(SfMessage,offset);
 F(SfMessage,length);F(SfMessage,mode);F(SfMessage,data);
 L(SfFileStat);F(SfFileStat,bytes);F(SfFileStat,type);F(SfFileStat,mode);F(SfFileStat,flags);
 F(SfFileStat,size);F(SfFileStat,offset);
 L(SfDmaStatus);F(SfDmaStatus,size);F(SfDmaStatus,mode);F(SfDmaStatus,owner);F(SfDmaStatus,faults);
 F(SfDmaStatus,last_address);F(SfDmaStatus,probe_address);F(SfDmaStatus,source);F(SfDmaStatus,reason);
 F(SfDmaStatus,guard_intact);F(SfDmaStatus,active_mapping);
 L(SfBootArgs);F(SfBootArgs,size);F(SfBootArgs,abi_major);F(SfBootArgs,abi_minor);
 F(SfBootArgs,bundle);F(SfBootArgs,bundle_bytes);F(SfBootArgs,pid);F(SfBootArgs,manager);
 F(SfBootArgs,reply);F(SfBootArgs,service);F(SfBootArgs,stdin_cap);F(SfBootArgs,stdout_cap);
 F(SfBootArgs,stderr_cap);F(SfBootArgs,root_cap);F(SfBootArgs,argc);F(SfBootArgs,argv_offset);
 F(SfBootArgs,envc);F(SfBootArgs,env_offset);F(SfBootArgs,dma_virtual);F(SfBootArgs,dma_physical);
 F(SfBootArgs,device_io);F(SfBootArgs,device_bdf);F(SfBootArgs,device_rights);
 L(SfBootSegment);F(SfBootSegment,physical);F(SfBootSegment,rva);F(SfBootSegment,file_bytes);
 F(SfBootSegment,memory_bytes);F(SfBootSegment,flags);F(SfBootSegment,reserved);
 L(SfBootInfo);F(SfBootInfo,magic);F(SfBootInfo,boot_type);F(SfBootInfo,framebuffer_base);
 F(SfBootInfo,framebuffer_size);F(SfBootInfo,width);F(SfBootInfo,height);F(SfBootInfo,pitch);F(SfBootInfo,bpp);
 F(SfBootInfo,memory_map);F(SfBootInfo,memory_map_size);F(SfBootInfo,memory_map_descriptor_size);
 F(SfBootInfo,memory_map_entry_count);F(SfBootInfo,acpi_rsdp);F(SfBootInfo,efi_runtime_services);
 F(SfBootInfo,total_memory);F(SfBootInfo,initrd_addr);F(SfBootInfo,initrd_size);F(SfBootInfo,size);
 F(SfBootInfo,version);F(SfBootInfo,init_segment_count);F(SfBootInfo,reserved);F(SfBootInfo,init_entry);
 F(SfBootInfo,bundle);F(SfBootInfo,bundle_bytes);F(SfBootInfo,init_segments);
 L(SfLoadSegment);F(SfLoadSegment,rva);F(SfLoadSegment,bytes);F(SfLoadSegment,data);
 F(SfLoadSegment,flags);F(SfLoadSegment,reserved);
 L(SfArguments);F(SfArguments,count);F(SfArguments,bytes);F(SfArguments,env_count);
 F(SfArguments,reserved);F(SfArguments,data);
 L(SfLoadPlan);F(SfLoadPlan,size);F(SfLoadPlan,count);F(SfLoadPlan,entry);F(SfLoadPlan,image_base);
 F(SfLoadPlan,grant_manager);F(SfLoadPlan,grant_service);F(SfLoadPlan,service_rights);F(SfLoadPlan,grant_bundle);
 F(SfLoadPlan,grant_device);F(SfLoadPlan,device_rights);F(SfLoadPlan,segments);F(SfLoadPlan,arguments);
 L(SfNativeSegment);F(SfNativeSegment,rva);F(SfNativeSegment,memory);F(SfNativeSegment,offset);
 F(SfNativeSegment,bytes);F(SfNativeSegment,flags);F(SfNativeSegment,reserved);
 L(SfNativePlan);F(SfNativePlan,entry);F(SfNativePlan,image_base);F(SfNativePlan,relocation_offset);
 F(SfNativePlan,count);F(SfNativePlan,profile);F(SfNativePlan,relocation_count);
 F(SfNativePlan,reserved);F(SfNativePlan,segments);
 L(SfNetInfo);F(SfNetInfo,mac);F(SfNetInfo,reserved);F(SfNetInfo,ip);F(SfNetInfo,gateway);
 F(SfNetInfo,netmask);F(SfNetInfo,rx_frames);F(SfNetInfo,tx_frames);F(SfNetInfo,link);
 L(SfNetPacket);F(SfNetPacket,ip);F(SfNetPacket,port);F(SfNetPacket,bytes);F(SfNetPacket,data);
 L(SfTrustKey);F(SfTrustKey,public_key);F(SfTrustKey,usage);F(SfTrustKey,revoked);
 puts("}}");return 0;
}
