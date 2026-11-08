#ifndef ACCOUNTS_H
#define ACCOUNTS_H
#include "sf.h"
/* Bounded account database embedded in the signed sf-init package.
 * Names are restricted to lower-case ASCII so that a home path is always a
 * canonical namespace path, and passwords never leave the build host in clear
 * text: the generated table stores a salted iterative SHA-256 digest. */
#define SF_ACCOUNT_MAX 8
#define SF_ACCOUNT_NAME_MAX 8
#define SF_ACCOUNT_SALT_BYTES 16
#define SF_ACCOUNT_HASH_BYTES 32
#define SF_ACCOUNT_HASH_ROUNDS 256
#define SF_ACCOUNT_PASSWORD_MAX 32
#define SF_ACCOUNT_TEXT_MAX 1024
#define SF_ACCOUNT_HOME_MAX 96
typedef struct {char name[SF_ACCOUNT_NAME_MAX+1];uint16_t uid,gid;uint8_t salt[SF_ACCOUNT_SALT_BYTES];uint8_t hash[SF_ACCOUNT_HASH_BYTES];} SfAccount;
typedef struct {SfAccount accounts[SF_ACCOUNT_MAX];unsigned count;} SfAccountStore;
/* Parse name:uid:gid:salt_hex:hash_hex lines. Blank and '#' lines are ignored.
 * The store is rejected unless exactly one uid-0 "root" account exists. */
int sf_accounts_parse(const char *text,size_t bytes,SfAccountStore *out);
int sf_accounts_load_builtin(SfAccountStore *out);
int sf_account_lookup(const SfAccountStore *store,const char *name,const SfAccount **out);
int sf_account_by_uid(const SfAccountStore *store,uint32_t uid,const SfAccount **out);
int sf_account_verify(const SfAccount *account,const char *password);
int sf_account_home(const SfAccount *account,char *out,size_t size);
#endif
