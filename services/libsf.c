#include "libsf.h"
void sf_number(uint64_t n){char b[24];unsigned i=23;b[i]=0;do{b[--i]='0'+n%10;n/=10;}while(n);sf_log(b+i);}
void sf_assert(int ok,const char*s){sf_log(ok?"[PASS] ":"[FAIL] ");sf_log(s);sf_log("\n");if(!ok){sf_sys(SYS_SHUTDOWN,1,0,0);for(;;)sf_yield();}}
