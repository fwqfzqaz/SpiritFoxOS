#ifndef SF_SCHED_POLICY_H
#define SF_SCHED_POLICY_H
#include "sf.h"
typedef struct {uint64_t ready_since;uint32_t priority,ready;} SfSchedEntry;
int sf_sched_pick(const SfSchedEntry*,unsigned,int,uint64_t);
#endif
