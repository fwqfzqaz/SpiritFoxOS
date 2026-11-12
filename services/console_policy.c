#include "console_policy.h"
struct SfConsolePolicy {SfAccountStore accounts;SfSessionTable sessions;};
static SfConsolePolicy console_policy;
SfConsolePolicy *sf_console_policy_default(void){return &console_policy;}
static int attach_root(SfConsolePolicy *p,int status){
 const SfAccount *root=0;
 if(!status)status=sf_account_lookup(&p->accounts,"root",&root);
 if(!status)status=sf_session_boot(&p->sessions,root);
 if(status)memset(p,0,sizeof(*p));return status;
}
int sf_console_policy_init(SfConsolePolicy *p){
 if(!p)return SF_INVALID;memset(p,0,sizeof(*p));
 return attach_root(p,sf_accounts_load_builtin(&p->accounts));
}
#ifdef SF_HOST_TEST
int sf_console_policy_fixture(SfConsolePolicy *p,const char *text,size_t size){
 if(!p)return SF_INVALID;memset(p,0,sizeof(*p));
 return attach_root(p,sf_accounts_parse(text,size,&p->accounts));
}
#endif
const SfAccountStore *sf_console_accounts(const SfConsolePolicy *p){return p?&p->accounts:0;}
const SfSessionTable *sf_console_sessions(const SfConsolePolicy *p){return p?&p->sessions:0;}
int sf_console_identity(const SfConsolePolicy *p,uint32_t *uid,uint32_t *gid,uint32_t *owner){
 const SfSession *s=p?sf_session_current(&p->sessions):0;if(!s)return SF_DENIED;
 /* Copy credentials at admission; a later console switch cannot relabel an
  * existing process. Failure never partially writes the caller's snapshot. */
 if(uid)*uid=s->uid;if(gid)*gid=s->gid;if(owner)*owner=sf_session_owner(sf_session_active(&p->sessions));return 0;
}
static int administrative(const char *line){
 static const char *const names[]={"namespace-recovery-test","namespace-status","namespace-crash","namespace-recover","persist","dma-test ","disk-crash","install ","trust","priority-test","priority ","ipc-stats-test","threads-test","threads ","page-quota-test","cap-limit-test","ipc-cancel-test","page-limit ","cap-limit ","cpu-budget ","cpu-state-test","revoke-key ","channel-test","echo-channel-test","echo-capacity-test","native-capacity-test","native-drain-test","native-crash","native-hang","native-bad-reply","native-bad-op","start native","stop native","echo-crash","echo-hang","echo-bad-reply","echo-bad-op","start echo","stop echo","mount /disk","unmount /disk","start disk","stop disk","dhcp","gui","selftest","shutdown"};
 for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++){size_t n=strlen(names[i]);if(!strcmp(line,names[i])||(names[i][n-1]==' '&&!strncmp(line,names[i],n)))return 1;}
 return 0;
}
int sf_console_authorize(const SfConsolePolicy *p,const char *line){
 if(!line)return SF_INVALID;
 const SfSession *s=p?sf_session_current(&p->sessions):0;if(!s)return SF_DENIED;
 return administrative(line)&&s->uid?SF_DENIED:0;
}
int sf_console_login(SfConsolePolicy *p,const char *name,const char *password,uint32_t *id){return p?sf_session_login(&p->sessions,&p->accounts,name,password,id):SF_INVALID;}
int sf_console_logout(SfConsolePolicy *p,uint32_t *ended){return p?sf_session_logout(&p->sessions,ended):SF_INVALID;}
