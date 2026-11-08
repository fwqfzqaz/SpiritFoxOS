#ifndef SESSION_H
#define SESSION_H
#include "sf.h"
#include "accounts.h"
/* The console is multiplexed, not forked: sf-init owns the only console read
 * capability, so bounded sessions keep independent identities while one of
 * them is attached to the console at a time. Session tags share the namespace
 * owner field with process ids, so the high bit keeps them disjoint. */
#define SF_SESSION_MAX 4
#define SF_SESSION_NONE 0u
#define SF_SESSION_OWNER_TAG 0x80000000u
typedef struct {uint32_t uid,gid;uint8_t used,authenticated;char name[SF_ACCOUNT_NAME_MAX+1];char home[SF_ACCOUNT_HOME_MAX+1];} SfSession;
typedef struct {SfSession slots[SF_SESSION_MAX];uint32_t active;} SfSessionTable;
void sf_session_init(SfSessionTable *table);
int sf_session_boot(SfSessionTable *table,const SfAccount *root);
int sf_session_login(SfSessionTable *table,const SfAccountStore *store,const char *name,const char *password,uint32_t *id);
int sf_session_logout(SfSessionTable *table,uint32_t *ended);
int sf_session_switch(SfSessionTable *table,uint32_t id);
uint32_t sf_session_active(const SfSessionTable *table);
const SfSession *sf_session_current(const SfSessionTable *table);
const SfSession *sf_session_by_id(const SfSessionTable *table,uint32_t id);
unsigned sf_session_count(const SfSessionTable *table);
int sf_session_is_root(const SfSessionTable *table);
uint32_t sf_session_owner(uint32_t id);
#endif
