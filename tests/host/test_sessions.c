#include <stdio.h>
#include "session.h"
static SfAccountStore store;static SfSessionTable sessions;
#define FIXTURE "root:0:0:f18371049203aa9e442c640df9630307:33823fd26bea823886981dc012a75be1a924f1caa1ac5e4e0fca5bd9d9b56b86\n" \
 "alice:1000:1000:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n" \
 "bob:1001:1001:b1b5734266f3e90733bc17d2d50a544f:1002a488c324bfa22af6275aa76941c322dff1b36861fc97134ffaf2ff1c1f1e\n"
#define CHECK(x) do{checks++;if(!(x)){printf("SESSIONS FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 unsigned checks=0;uint32_t id=0;const SfAccount*root=0,*alice=0;
 CHECK(!sf_accounts_parse(FIXTURE,strlen(FIXTURE),&store));CHECK(!sf_account_lookup(&store,"root",&root)&&!sf_account_lookup(&store,"alice",&alice));
 sf_session_init(&sessions);CHECK(!sf_session_count(&sessions)&&!sf_session_active(&sessions)&&!sf_session_current(&sessions));
 CHECK(sf_session_boot(&sessions,alice)==SF_DENIED&&!sf_session_count(&sessions));
 CHECK(!sf_session_boot(&sessions,root)&&sf_session_active(&sessions)==1&&sf_session_count(&sessions)==1);
 const SfSession*s=sf_session_current(&sessions);CHECK(s&&!strcmp(s->name,"root")&&s->uid==0&&s->gid==0&&!strcmp(s->home,"/home/root")&&s->authenticated);
 CHECK(sf_session_is_root(&sessions));CHECK(sf_session_owner(1)==0x80000001u&&sf_session_owner(4)==0x80000004u);
 CHECK(sf_session_login(&sessions,&store,"alice","wrong",&id)==SF_DENIED&&sf_session_active(&sessions)==1);
 CHECK(sf_session_login(&sessions,&store,"carol","x",&id)==SF_DENIED&&sf_session_active(&sessions)==1);
 CHECK(sf_session_login(&sessions,&store,"","alice",&id)==SF_INVALID);
 CHECK(sf_session_login(&sessions,&store,"alice","alice",&id)==0&&id==2&&sf_session_active(&sessions)==2&&sf_session_count(&sessions)==2);
 s=sf_session_current(&sessions);CHECK(s&&!strcmp(s->name,"alice")&&s->uid==1000&&s->gid==1000&&!strcmp(s->home,"/home/alice"));
 CHECK(!sf_session_login(&sessions,&store,"bob","bob",&id)&&id==3);
 CHECK(!sf_session_login(&sessions,&store,"alice","alice",&id)&&id==4);
 CHECK(sf_session_login(&sessions,&store,"bob","bob",&id)==SF_NO_SPACE&&sf_session_active(&sessions)==4);
 uint32_t ended=0;CHECK(!sf_session_logout(&sessions,&ended)&&ended==4&&sf_session_active(&sessions)==3);
 CHECK(!sf_session_switch(&sessions,1)&&sf_session_active(&sessions)==1&&sf_session_is_root(&sessions));
 CHECK(sf_session_switch(&sessions,0)==SF_NOT_FOUND&&sf_session_switch(&sessions,5)==SF_NOT_FOUND);
 CHECK(!sf_session_logout(&sessions,&ended)&&ended==1&&sf_session_active(&sessions)==3);
 CHECK(!sf_session_logout(&sessions,&ended)&&ended==3&&sf_session_active(&sessions)==2);
 CHECK(!sf_session_logout(&sessions,&ended)&&ended==2&&sf_session_active(&sessions)==0&&!sf_session_current(&sessions));
 CHECK(sf_session_logout(&sessions,&ended)==SF_NOT_FOUND);
 CHECK(!sf_session_by_id(&sessions,4)&&!sf_session_by_id(&sessions,0));
 CHECK(sf_session_switch(&sessions,3)==SF_NOT_FOUND&&!sf_session_current(&sessions));
 printf("SESSIONS PASS: %u checks; boot root, login/failure, bounded slots, logout reattachment\n",checks);return 0;
}
