#ifndef SF_CONSOLE_POLICY_H
#define SF_CONSOLE_POLICY_H
#include "accounts.h"
#include "session.h"
/* Manager-local policy, not a kernel identity or an IPC/serialized layout.
 * Console input/output and namespace home creation remain orchestration. */
typedef struct SfConsolePolicy SfConsolePolicy;
SfConsolePolicy *sf_console_policy_default(void);
int sf_console_policy_init(SfConsolePolicy *);
const SfAccountStore *sf_console_accounts(const SfConsolePolicy *);
const SfSessionTable *sf_console_sessions(const SfConsolePolicy *);
int sf_console_identity(const SfConsolePolicy *,uint32_t *uid,uint32_t *gid,uint32_t *owner);
int sf_console_authorize(const SfConsolePolicy *,const char *canonical_command);
int sf_console_login(SfConsolePolicy *,const char *,const char *,uint32_t *id);
int sf_console_logout(SfConsolePolicy *,uint32_t *ended);
#ifdef SF_HOST_TEST
int sf_console_policy_fixture(SfConsolePolicy *,const char *,size_t);
#endif
#endif
