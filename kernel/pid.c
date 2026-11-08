#include "pid.h"
/* PID 分配：从单调递增计数器取号，越过 SF_PID_MAX 报配额耗尽。
 * 计数器永不回退，保证 PID 单调，杜绝旧 PID 复用歧义。 */
long sf_pid_take(uint32_t*next){
 if(!next)return SF_INVALID;uint32_t pid=*next;
 if(!pid||pid>SF_PID_MAX)return SF_NO_SPACE;
 *next=pid+1;return pid;
}
