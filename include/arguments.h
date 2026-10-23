#ifndef SF_ARGUMENTS_H
#define SF_ARGUMENTS_H
#include "sf.h"
int sf_arguments_validate(const SfArguments *);
int sf_arguments_pack(SfArguments *,const char *const *,unsigned);
int sf_arguments_split(char *,char **,unsigned);
int sf_boot_arguments_validate(const SfBootArgs *,size_t);
const char *sf_argument(const SfBootArgs *,size_t,unsigned);
#endif
