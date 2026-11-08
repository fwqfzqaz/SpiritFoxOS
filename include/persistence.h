#ifndef SF_PERSISTENCE_H
#define SF_PERSISTENCE_H
#include "storage.h"
#include "namespace.h"
/* sf-init 侧客户端：持久化卷与更新状态机运行在 sf-persist.sfmod，
 * 本头文件只暴露有界 RPC 与命名空间投影。 */
int sf_persist_start(SfModuleSpawn spawn);
void sf_persist_attach(void);
int sf_persist_ready(void);
void sf_persist_driver_down(void);
int sf_persist_mount(SfNamespace*);
/* 0 stages an update; 1 means the exact confirmed package is already present. */
int sf_persist_install(const char*);
int sf_persist_begin(void);
int sf_persist_finish(int success);
long sf_persist_package(void*,size_t);
/* Read-only authenticated preview; execution still requires begin + package. */
long sf_persist_preview(void*,size_t);
/* 0 revokes a stored package's signer; 1 means it was already revoked. */
int sf_persist_revoke(const char*);
void sf_persist_trust_status(void);
void sf_persist_status(void);
#endif
