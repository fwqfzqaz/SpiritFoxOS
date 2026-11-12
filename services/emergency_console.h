#ifndef SF_EMERGENCY_CONSOLE_H
#define SF_EMERGENCY_CONSOLE_H
#include "sf.h"
/* Root policy state: one bounded output token per registered child, separate
 * from namespace handles (whose slot byte is only 1..128). Never a directory. */
typedef struct {uint32_t owner,handle;} SfEmergencyConsole;
int64_t sf_emergency_console(SfEmergencyConsole*,uint32_t*,uint32_t,SfMessage*,long(*)(const void*,size_t));
#endif
