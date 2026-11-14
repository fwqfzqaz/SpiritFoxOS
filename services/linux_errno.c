/* SpiritFoxOS 状态码到 Linux errno 的唯一映射表（S3）。
 *
 * 纯函数、无外部依赖：翻译器把它编译进兼容 blob，宿主测试直接链接本文件，
 * 以保证“宿主 errno 表全覆盖”验收与运行期行为使用同一份实现。 */
#include "linux_abi.h"

int sf_linux_errno(int64_t status){
 if(status>=0)return 0;
 switch(status){
  case -1 /* SF_INVALID */: return LX_EINVAL;
  case -2 /* SF_DENIED */: return LX_EACCES;
  case -3 /* SF_BAD_FORMAT */: return LX_EINVAL;
  case -4 /* SF_BAD_SIGNATURE */: return LX_EACCES;
  case -5 /* SF_ABI_MISMATCH */: return LX_ENOSYS;
  case -6 /* SF_DEPENDENCY */: return LX_ENOSYS;
  case -7 /* SF_BUSY */: return LX_EBUSY;
  case -8 /* SF_DRAINING */: return LX_EBUSY;
  case -9 /* SF_TIMEOUT */: return LX_ETIMEDOUT;
  case -10 /* SF_DEVICE_GONE */: return LX_ENODEV;
  case -11 /* SF_NOT_SEEKABLE */: return LX_ESPIPE;
  case -12 /* SF_STALE_HANDLE */: return LX_EBADF;
  case -13 /* SF_REVOKED */: return LX_EACCES;
  case -14 /* SF_RESTART_REQUIRED */: return LX_EIO;
  case -15 /* SF_WOULD_BLOCK */: return LX_EAGAIN;
  case -16 /* SF_NOT_FOUND */: return LX_ENOENT;
  case -17 /* SF_READ_ONLY */: return LX_EROFS;
  case -18 /* SF_NO_SPACE */: return LX_ENOSPC;
  case -19 /* SF_EVENT_OVERFLOW */: return LX_EIO;
  case -20 /* SF_COMMIT_UNCERTAIN */: return LX_EIO;
  case -21 /* SF_VERSION_REJECTED */: return LX_ENOSYS;
  default: return LX_EIO;
 }
}
