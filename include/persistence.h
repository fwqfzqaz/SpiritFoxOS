#ifndef SF_PERSISTENCE_H
#define SF_PERSISTENCE_H
#include "namespace.h"
int sf_persist_mount(SfNamespace*);
/* 0 stages an update; 1 means the exact confirmed package is already present. */
int sf_persist_install(const char*);
int sf_persist_begin(void);
int sf_persist_finish(int success);
long sf_persist_package(void*,size_t);
/* 0 revokes a stored package's signer; 1 means it was already revoked. */
int sf_persist_revoke(const char*);
void sf_persist_trust_status(void);
void sf_persist_status(void);
#endif
