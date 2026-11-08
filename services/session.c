#include "session.h"
static int create_session(SfSessionTable *table,const SfAccount *account,uint32_t *id){
 if(!table||!account)return SF_INVALID;
 for(unsigned i=0;i<SF_SESSION_MAX;i++){
  SfSession *slot=&table->slots[i];if(slot->used)continue;
  SfSession value;memset(&value,0,sizeof(value));value.used=1;value.authenticated=1;value.uid=account->uid;value.gid=account->gid;
  memcpy(value.name,account->name,strlen(account->name)+1);
  if(sf_account_home(account,value.home,sizeof(value.home)))return SF_BAD_FORMAT;
  *slot=value;table->active=i+1;if(id)*id=i+1;return 0;
 }
 return SF_NO_SPACE;
}
void sf_session_init(SfSessionTable *table){if(table)memset(table,0,sizeof(*table));}
int sf_session_boot(SfSessionTable *table,const SfAccount *root){
 if(!root||root->uid||root->gid)return SF_DENIED;
 return create_session(table,root,0);
}
int sf_session_login(SfSessionTable *table,const SfAccountStore *store,const char *name,const char *password,uint32_t *id){
 if(!table||!store||!name||!password||!*name)return SF_INVALID;
 const SfAccount *account=0;if(sf_account_lookup(store,name,&account))return SF_DENIED;
 if(sf_account_verify(account,password))return SF_DENIED;
 uint32_t used=0;for(unsigned i=0;i<SF_SESSION_MAX;i++)if(table->slots[i].used)used++;
 if(used>=SF_SESSION_MAX)return SF_NO_SPACE;
 return create_session(table,account,id);
}
int sf_session_logout(SfSessionTable *table,uint32_t *ended){
 if(!table)return SF_INVALID;uint32_t id=table->active;if(!id||id>SF_SESSION_MAX||!table->slots[id-1].used)return SF_NOT_FOUND;
 memset(&table->slots[id-1],0,sizeof(table->slots[id-1]));(void)ended;
 if(ended)*ended=id;
 table->active=SF_SESSION_NONE;
 for(unsigned i=SF_SESSION_MAX;i;i--)if(table->slots[i-1].used){table->active=i;break;}
 return 0;
}
int sf_session_switch(SfSessionTable *table,uint32_t id){
 if(!table)return SF_INVALID;if(!id||id>SF_SESSION_MAX||!table->slots[id-1].used)return SF_NOT_FOUND;
 table->active=id;return 0;
}
uint32_t sf_session_active(const SfSessionTable *table){return table?table->active:SF_SESSION_NONE;}
const SfSession *sf_session_current(const SfSessionTable *table){const SfSession *s=table?sf_session_by_id(table,table->active):0;return s?s:0;}
const SfSession *sf_session_by_id(const SfSessionTable *table,uint32_t id){
 if(!table||!id||id>SF_SESSION_MAX||!table->slots[id-1].used)return 0;return&table->slots[id-1];
}
unsigned sf_session_count(const SfSessionTable *table){unsigned count=0;if(table)for(unsigned i=0;i<SF_SESSION_MAX;i++)if(table->slots[i].used)count++;return count;}
int sf_session_is_root(const SfSessionTable *table){const SfSession *s=sf_session_current(table);return s&&!s->uid;}
uint32_t sf_session_owner(uint32_t id){return SF_SESSION_OWNER_TAG|id;}
