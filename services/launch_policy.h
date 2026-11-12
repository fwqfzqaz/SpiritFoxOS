#ifndef SF_LAUNCH_POLICY_H
#define SF_LAUNCH_POLICY_H
#include "package.h"
#include "arguments.h"
#define SF_LAUNCH_LIBRARY_FILE "sf-libsf.sfmod"
/* Pure manager admission over an already authenticated package. This module
 * has no account/session, kernel capability, namespace or process ownership. */
int sf_launch_admit(const SfPackage *,uint16_t expected_kind,int directory,const SfArguments *);
int sf_launch_library_request(const SfPackage *);
int sf_launch_library_admit(const SfPackage *);
#endif
