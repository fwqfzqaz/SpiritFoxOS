#include "kernel.h"
/* Linux 兼容描述符：入口必须落在本进程 RX 页，帧页必须是整页可写且不可执行。
 * 描述符只有一份，存放在主线程；重复注册返回 SF_BUSY。 */
int sf_compat_register(Task*t,uint64_t handler,uint64_t frame,uint64_t flags){
 if(!t||flags||(frame&(SF_PAGE-1)))return SF_INVALID;
 if(!t->main)return SF_DENIED;
 Task*m=task_main(t);
 if(m->compat_handler)return SF_BUSY;
 if(!user_exec(t,handler))return SF_DENIED;
 if(!user_range(t,frame,SF_PAGE,1)||user_exec(t,frame))return SF_DENIED;
 m->compat_handler=handler;m->compat_frame=frame;
 return SF_OK;
}
/* 保存帧来自 CPU 进入 int 0x80 时的用户上下文；这里再确认它不是内核目标。 */
int sf_compat_frame_valid(const Frame*f){
 if(!f||f->cs!=0x1b||f->ss!=0x23||f->rip>=0x80000000ull)return 0;
 return !(f->rsp>=0x0000800000000000ull&&f->rsp<0xffff800000000000ull);
}
