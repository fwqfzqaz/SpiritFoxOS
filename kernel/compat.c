#include "kernel.h"
/* Linux 兼容描述符：入口必须落在本进程 RX 页，帧页必须是整页可写且不可执行，
 * 翻译器栈顶必须可写。注册只允许主线程进行一次；入口、帧页与栈都记录在
 * 当前线程，同级线程由内核在创建时复制入口并分配各自的帧页与栈。 */
int sf_compat_register(Task*t,uint64_t handler,uint64_t frame,uint64_t flags,uint64_t stack){
 if(!t||flags||(frame&(SF_PAGE-1)))return SF_INVALID;
 if(!t->main)return SF_DENIED;
 Task*m=task_main(t);
 if(m->compat_handler)return SF_BUSY;
 if(!user_exec(t,handler))return SF_DENIED;
 if(!user_range(t,frame,SF_PAGE,1)||user_exec(t,frame))return SF_DENIED;
 if(!stack||(stack&15)||!user_range(t,stack-1,1,1))return SF_DENIED;
 t->compat_handler=handler;t->compat_frame=frame;t->compat_stack=stack;
 m->compat_handler=handler;
 return SF_OK;
}
/* 保存帧来自 CPU 进入 int 0x80 时的用户上下文；这里再确认它不是内核目标。 */
int sf_compat_frame_valid(const Frame*f){
 if(!f||f->cs!=0x1b||f->ss!=0x23||f->rip>=0x80000000ull)return 0;
 return !(f->rsp>=0x0000800000000000ull&&f->rsp<0xffff800000000000ull);
}
