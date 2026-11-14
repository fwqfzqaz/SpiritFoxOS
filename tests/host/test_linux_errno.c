/* E1：宿主 errno 表全覆盖。直接链接 services/linux_errno.c，确保验收与运行期
 * 翻译器使用同一份映射实现。 */
#include <stdio.h>
#include "linux_abi.h"

int sf_linux_errno(int64_t status);

static unsigned checks;
#define CHECK(status,wanted) do{checks++;int got=sf_linux_errno(status);\
 if(got!=(wanted)){printf("LINUX ERRNO FAIL status=%lld got=%d want=%d\n",\
 (long long)(status),got,(wanted));return 1;}}while(0)

int main(void){
 CHECK(0,0);
 CHECK(1,0);
 CHECK(4096,0);
 /* SpiritFoxOS 全部负状态码（SF_INVALID..SF_VERSION_REJECTED）。 */
 CHECK(-1,LX_EINVAL);
 CHECK(-2,LX_EACCES);
 CHECK(-3,LX_EINVAL);
 CHECK(-4,LX_EACCES);
 CHECK(-5,LX_ENOSYS);
 CHECK(-6,LX_ENOSYS);
 CHECK(-7,LX_EBUSY);
 CHECK(-8,LX_EBUSY);
 CHECK(-9,LX_ETIMEDOUT);
 CHECK(-10,LX_ENODEV);
 CHECK(-11,LX_ESPIPE);
 CHECK(-12,LX_EBADF);
 CHECK(-13,LX_EACCES);
 CHECK(-14,LX_EIO);
 CHECK(-15,LX_EAGAIN);
 CHECK(-16,LX_ENOENT);
 CHECK(-17,LX_EROFS);
 CHECK(-18,LX_ENOSPC);
 CHECK(-19,LX_EIO);
 CHECK(-20,LX_EIO);
 CHECK(-21,LX_ENOSYS);
 /* 未定义状态码必须保守回落到 EIO，而不是 0 或随机值。 */
 CHECK(-22,LX_EIO);
 CHECK(-1000,LX_EIO);
 printf("LINUX ERRNO TABLE PASSED (%u checks)\n",checks);
 return 0;
}
