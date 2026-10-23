#ifndef SF_STORAGE_H
#define SF_STORAGE_H
#include "manager.h"
#include "namespace.h"
#include "readfs.h"
typedef int (*SfModuleSpawn)(const char*,uint32_t,uint32_t,uint32_t);
void sf_storage_init(SfManager*,SfNamespace*,uint32_t,SfModuleSpawn);
int sf_storage_start(void);
int sf_storage_mount(void);
int sf_storage_unmount(void);
int sf_storage_stop(void);
long sf_storage_load(const char*,void*,size_t);
int sf_storage_ready(void);
void sf_storage_tests(void);
#endif
