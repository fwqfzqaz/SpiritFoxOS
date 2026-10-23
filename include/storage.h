#ifndef SF_STORAGE_H
#define SF_STORAGE_H
#include "manager.h"
#include "namespace.h"
#include "readfs.h"
typedef int (*SfModuleSpawn)(const char*,uint32_t,uint32_t,uint32_t,uint32_t);
void sf_storage_init(SfManager*,SfNamespace*,uint32_t,SfModuleSpawn);
int sf_storage_start(void);
int sf_storage_mount(void);
int sf_storage_unmount(void);
int sf_storage_stop(void);
long sf_storage_load(const char*,void*,size_t);
int sf_storage_ready(void);
void sf_storage_tests(void);
int sf_storage_writable(void);
int sf_storage_sector_read(void*,uint64_t,uint8_t[512]);
int sf_storage_sector_write(void*,uint64_t,const uint8_t[512]);
int sf_storage_flush(void*);
int sf_storage_write_range(uint64_t,uint64_t);
int sf_storage_transaction(int);
void sf_storage_write_tests(void);
void sf_storage_dma_probe(unsigned);
#endif
