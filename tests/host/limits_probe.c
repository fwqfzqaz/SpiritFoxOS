/* 上限清单探针：从源码常量生成容量基线，避免文档与代码漂移。
 * tools/limits.py 编译并运行本文件，输出 LIMIT name=value 行。 */
#include <stdio.h>
#include "sf.h"
#include "namespace.h"
#include "sfrw.h"
#include "net.h"

int main(void){
 printf("LIMIT sf_max_tasks=%u\n",SF_MAX_TASKS);
 printf("LIMIT sf_max_threads=%u\n",SF_MAX_THREADS);
 printf("LIMIT sf_max_cpus=%u\n",SF_MAX_CPUS);
 printf("LIMIT sf_cap_limit_max=%u\n",SF_CAP_LIMIT_MAX);
 printf("LIMIT sf_page_limit_max=%u\n",SF_PAGE_LIMIT_MAX);
 printf("LIMIT sf_kernel_pool_bytes=%llu\n",(unsigned long long)SF_PAGE_LIMIT_MAX*SF_PAGE);
 printf("LIMIT sf_msg_bytes=%u\n",SF_MSG_BYTES);
 printf("LIMIT sf_ns_node_max=%u\n",SF_NS_NODE_MAX);
 printf("LIMIT sf_ns_handle_max=%u\n",SF_NS_HANDLE_MAX);
 printf("LIMIT sf_ns_description_max=%u\n",SF_NS_DESCRIPTION_MAX);
 printf("LIMIT sf_rw_max_blocks=%u\n",SF_RW_MAX_BLOCKS);
 printf("LIMIT sf_rw_max_volume_bytes=%llu\n",(unsigned long long)SF_RW_MAX_BLOCKS*SF_RW_BLOCK_BYTES);
 printf("LIMIT sf_rw_max_inodes=%u\n",SF_RW_MAX_INODES);
 printf("LIMIT sf_rw_file_max=%u\n",SF_RW_FILE_MAX);
 printf("LIMIT sf_rw_name_max=%u\n",SF_RW_NAME_MAX);
 printf("LIMIT sf_rw_path_max=%u\n",SF_RW_PATH_MAX);
 printf("LIMIT sf_rw_journal_max=%u\n",SF_RW_JOURNAL_MAX);
 printf("LIMIT sf_rw_quota_max=%u\n",SF_RW_QUOTA_MAX);
 printf("LIMIT sf_net_conn_max=%u\n",SF_NET_CONN_MAX);
 printf("LIMIT sf_net_payload_max=%u\n",SF_NET_CONN_PAYLOAD_MAX);
 printf("LIMIT sf_dma_bytes=%u\n",(unsigned)SF_DMA_BYTES);
 printf("LIMIT sf_fb_bytes=%u\n",(unsigned)SF_FB_BYTES);
 printf("LIMIT sf_image_max=%u\n",(unsigned)SF_IMAGE_MAX);
 return 0;
}
