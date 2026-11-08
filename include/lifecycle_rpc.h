#ifndef LIFECYCLE_RPC_H
#define LIFECYCLE_RPC_H
#include "sf.h"
#include "manager.h"
/* 生命周期 RPC：把模块状态机暴露为有界消息接口。调用者身份由内核在
 * SfMessage.flags 中提供，不接受请求自报；当前只允许根进程。
 * 返回 0 表示应把填好 status 的消息回复给调用者；非 0 表示消息头非法。 */
int sf_lifecycle_dispatch(SfManager*,uint32_t caller,uint32_t root,SfMessage*);
#endif
