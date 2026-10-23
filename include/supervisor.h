#ifndef SUPERVISOR_H
#define SUPERVISOR_H
#include "sf.h"
enum { SF_SUP_STOPPED,SF_SUP_WAITING,SF_SUP_STARTING,SF_SUP_RUNNING,SF_SUP_BLOCKED };
typedef struct {
 uint64_t retry_ticks,stable_ticks,next_tick,running_since;
 uint32_t max_attempts,attempts,state;
} SfSupervisor;
int sf_supervisor_init(SfSupervisor*,uint32_t,uint64_t,uint64_t);
void sf_supervisor_enable(SfSupervisor*,uint64_t,int);
void sf_supervisor_disable(SfSupervisor*);
int sf_supervisor_failure(SfSupervisor*,uint64_t);
int sf_supervisor_begin(SfSupervisor*,uint64_t);
int sf_supervisor_started(SfSupervisor*,uint64_t);
#endif
