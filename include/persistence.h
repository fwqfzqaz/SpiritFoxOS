#ifndef SF_PERSISTENCE_H
#define SF_PERSISTENCE_H
#include "namespace.h"
int sf_persist_mount(SfNamespace*);
int sf_persist_install(const char*);
int sf_persist_begin(void);
int sf_persist_finish(int success);
long sf_persist_package(void*,size_t);
void sf_persist_status(void);
#endif
