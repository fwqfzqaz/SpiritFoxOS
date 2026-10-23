#ifndef SF_VERSION_H
#define SF_VERSION_H
#include "sf.h"
typedef struct { uint32_t part[3]; } SfVersion;
int sf_version_parse(SfVersion*,const char*);
int sf_version_compare(const SfVersion*,const SfVersion*);
/* 0: upgrade, 1: exact reinstall, SF_VERSION_REJECTED: downgrade/replacement. */
int sf_version_upgrade(const SfVersion*,const uint8_t current[32],const SfVersion*,const uint8_t candidate[32]);
#endif
