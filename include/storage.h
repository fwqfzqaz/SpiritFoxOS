#ifndef SF_STORAGE_H
#define SF_STORAGE_H
#include "manager.h"
#include "namespace.h"
#include "readfs.h"
typedef int (*SfModuleSpawn)(const char*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
#define SF_STORAGE_SNAPSHOT_BYTES (128u*1024u)
void sf_storage_init(SfManager*,SfNamespace*,SfModuleSpawn);
void sf_storage_namespace_bind(SfNamespace*);
uint32_t sf_storage_namespace_pid(void);
int sf_storage_namespace_reset(void);
int sf_storage_namespace_restore(void);
int sf_storage_namespace_crash(void);
int sf_storage_start(void);
int sf_storage_restart_next(void);
int sf_storage_mount(void);
int sf_storage_unmount(void);
int sf_storage_stop(void);
long sf_storage_load(const char*,void*,size_t);
/* 单线程共享装载缓冲：包快照与只读磁盘文件缓存互斥使用。
 * 取得缓冲即作废磁盘缓存，避免调用方覆写后旧缓存继续命中。 */
uint8_t* sf_storage_scratch(void);
int sf_storage_ready(void);
void sf_storage_tests(void);
int sf_storage_writable(void);
int sf_storage_sector_read(void*,uint64_t,uint8_t[512]);
int sf_storage_sector_write(void*,uint64_t,const uint8_t[512]);
int sf_storage_flush(void*);
int sf_storage_write_range(uint64_t,uint64_t);
int sf_storage_transaction(int);
/* Serve one sf-fs block request; called by sf-init whenever it waits for IPC. */
int sf_storage_broker(unsigned);
/* 供 sf-persist 客户端派生能力并转交块端点所有权。 */
int sf_storage_driver(uint32_t*,uint32_t*,uint32_t*,uint64_t*,int*);
void sf_storage_write_tests(void);
void sf_storage_dma_probe(unsigned);
int sf_storage_crash_test(void);
#endif
