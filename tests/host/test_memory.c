#include <stdio.h>
#include "../../kernel/kernel.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("Memory FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 Task task={0};task.pid=17;task.cr3=space_new(task.pid);CHECK(task.cr3);
 uint8_t*a=page_alloc(task.pid),*b=page_alloc(task.pid),*ro=page_alloc(task.pid);CHECK(a&&b&&ro);
 CHECK(!space_map(&task,SF_BASE,(uint64_t)a,3));CHECK(!space_map(&task,SF_BASE+4096,(uint64_t)b,3));CHECK(!space_map(&task,SF_ARGS,(uint64_t)ro,1));
 for(unsigned i=0;i<16384;i+=4096)CHECK(!space_map_mmio(&task,SF_DEVICE_MMIO+i,0xc0000000ull+i));
 uint8_t data[32],out[32];memset(data,0x5a,sizeof(data));memset(out,0,sizeof(out));
 CHECK(!copy_out(&task,SF_BASE+4080,data,sizeof(data)));CHECK(!memcmp(a+4080,data,16)&&!memcmp(b,data,16));
 CHECK(!copy_in(&task,out,SF_BASE+4080,sizeof(out))&&!memcmp(data,out,sizeof(data)));
 CHECK(!copy_in(&task,out,SF_ARGS,sizeof(out)));CHECK(copy_out(&task,SF_ARGS,data,sizeof(data))==SF_DENIED);
 CHECK(!user_range(&task,0x100000,1,0));CHECK(!user_range(&task,UINT64_MAX,2,0));CHECK(!user_range(&task,SF_BASE,UINT64_MAX,0));CHECK(!user_range(&task,0x80000000ull-1,2,0));
 for(unsigned off=0;off<16384;off+=4096){CHECK(!user_range(&task,SF_DEVICE_MMIO+off,1,0));CHECK(!user_range(&task,SF_DEVICE_MMIO+off,1,1));CHECK(copy_in(&task,out,SF_DEVICE_MMIO+off,1)==SF_DENIED);CHECK(copy_out(&task,SF_DEVICE_MMIO+off,data,1)==SF_DENIED);}
 /* Whole-range validation must reject a later device page before modifying
    an earlier ordinary RAM page. Physical BAR addresses must never be read. */
 uint8_t*edge=page_alloc(task.pid);CHECK(edge);CHECK(!space_map(&task,SF_DEVICE_MMIO-4096,(uint64_t)edge,3));
 CHECK(copy_out(&task,SF_DEVICE_MMIO-16,data,sizeof(data))==SF_DENIED);CHECK(edge[4095]==0);CHECK(copy_in(&task,out,SF_DEVICE_MMIO-16,sizeof(out))==SF_DENIED);
 CHECK(!space_map(&task,SF_DMA,(uint64_t)page_alloc(task.pid),3));CHECK(!copy_out(&task,SF_DMA,data,sizeof(data)));CHECK(!copy_in(&task,out,SF_DMA,sizeof(out))&&!memcmp(out,data,sizeof(out)));
 pages_free(task.pid);printf("Memory tests passed: %u assertions\n",checks);return 0;
}
