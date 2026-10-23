#include "pid.h"
long sf_pid_take(uint32_t*next){
 if(!next)return SF_INVALID;uint32_t pid=*next;
 if(!pid||pid>SF_PID_MAX)return SF_NO_SPACE;
 *next=pid+1;return pid;
}
