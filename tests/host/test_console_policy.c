#include <stdio.h>
#include "console_policy.h"
#define FIXTURE "root:0:0:f18371049203aa9e442c640df9630307:33823fd26bea823886981dc012a75be1a924f1caa1ac5e4e0fca5bd9d9b56b86\n" \
 "alice:1000:1000:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n" \
 "bob:1001:1001:b1b5734266f3e90733bc17d2d50a544f:1002a488c324bfa22af6275aa76941c322dff1b36861fc97134ffaf2ff1c1f1e\n"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("CONSOLE POLICY FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 SfConsolePolicy *p=sf_console_policy_default();
 uint32_t uid=71,gid=72,owner=73,id=74,ended=75;
 CHECK(p&&sf_console_policy_default()==p);
 CHECK(sf_console_policy_init(0)==SF_INVALID);
 CHECK(!sf_console_accounts(0)&&!sf_console_sessions(0));
 CHECK(sf_console_identity(0,&uid,&gid,&owner)==SF_DENIED&&uid==71&&gid==72&&owner==73);
 CHECK(sf_console_authorize(p,0)==SF_INVALID);
 CHECK(sf_console_policy_fixture(p,"invalid",7)==SF_BAD_FORMAT);
 CHECK(!sf_session_count(sf_console_sessions(p))&&!sf_console_accounts(p)->count);
 CHECK(sf_console_authorize(p,"shutdown")==SF_DENIED);
 CHECK(!sf_console_policy_fixture(p,FIXTURE,strlen(FIXTURE)));
 CHECK(sf_console_accounts(p)->count==3&&sf_session_count(sf_console_sessions(p))==1);
 CHECK(!sf_console_identity(p,&uid,&gid,&owner)&&!uid&&!gid&&owner==0x80000001u);
 CHECK(!sf_console_identity(p,0,0,0));
 const char *admin[]={"namespace-status","namespace-crash","namespace-recover","namespace-recovery-test","persist","install /disk/bin/x.sfk","revoke-key /disk/pkg","priority 2 3","threads 2 1","page-limit 2 0","cap-limit 2 1","cpu-budget 2 0","ipc-cancel-test","ipc-stats-test","dma-test 1","disk-crash","trust","channel-test","native-capacity-test","echo-channel-test","start native","stop native","native-crash","native-hang","native-bad-reply","native-bad-op","start echo","stop echo","echo-crash","echo-hang","echo-bad-reply","echo-bad-op","mount /disk","unmount /disk","start disk","stop disk","dhcp","gui","selftest","shutdown"};
 const char *ordinary[]={"run hello.sfk","runat /home/alice app.sfk","runenv X=1 -- hello.sfk","ls /home/alice","cat /home/alice/x","write /home/alice/x hi","stat /tmp","rm /tmp/x","mkdir /tmp/x","rmdir /tmp/x","echo","ifconfig","ping 10.0.2.2","udp 10.0.2.2 1 hi","tcp 10.0.2.2 1 hi","open 10.0.2.2 1","send 1 hi","recv 1","close 1","conns","","shutdownx","persistx","installx x","start nativex"};
 for(unsigned i=0;i<sizeof(admin)/sizeof(*admin);i++)CHECK(!sf_console_authorize(p,admin[i]));
 CHECK(!sf_console_authorize(p,"file-peer-test"));
 CHECK(!sf_console_login(p,"alice","alice",&id)&&id==2);
 CHECK(!sf_console_identity(p,&uid,&gid,&owner)&&uid==1000&&gid==1000&&owner==0x80000002u);
 /* Independent command vectors check the policy before handlers run. A
  * rejected command must not switch the active session or mutate its table. */
 SfSessionTable before=*sf_console_sessions(p);
 for(unsigned i=0;i<sizeof(admin)/sizeof(*admin);i++)CHECK(sf_console_authorize(p,admin[i])==SF_DENIED);
 CHECK(sf_console_authorize(p,"file-peer-test")==SF_DENIED);
 for(unsigned i=0;i<sizeof(ordinary)/sizeof(*ordinary);i++)CHECK(!sf_console_authorize(p,ordinary[i]));
 CHECK(!memcmp(&before,sf_console_sessions(p),sizeof(before)));
 CHECK(sf_console_login(p,"alice","wrong",&id)==SF_DENIED&&id==2);
 CHECK(!memcmp(&before,sf_console_sessions(p),sizeof(before)));
 CHECK(!sf_console_login(p,"bob","bob",&id)&&id==3);
 CHECK(uid==1000&&gid==1000&&owner==0x80000002u); /* captured admission stays Alice */
 CHECK(!sf_console_identity(p,&uid,&gid,&owner)&&uid==1001&&owner==0x80000003u);
 CHECK(!sf_console_login(p,"alice","alice",&id)&&id==4);
 CHECK(sf_console_login(p,"bob","bob",&id)==SF_NO_SPACE&&id==4);
 for(unsigned i=4;i;i--)CHECK(!sf_console_logout(p,&ended)&&ended==i);
 CHECK(!sf_session_current(sf_console_sessions(p)));
 uid=71;gid=72;owner=73;
 CHECK(sf_console_identity(p,&uid,&gid,&owner)==SF_DENIED&&uid==71&&gid==72&&owner==73);
 for(unsigned i=0;i<sizeof(ordinary)/sizeof(*ordinary);i++)CHECK(sf_console_authorize(p,ordinary[i])==SF_DENIED);
 CHECK(sf_console_logout(p,&ended)==SF_NOT_FOUND&&ended==1);
 CHECK(!sf_console_login(p,"root","fox-root",&id)&&id==1);
 for(unsigned i=0;i<100;i++){
  CHECK(!sf_console_login(p,"alice","alice",&id)&&id==2);
  CHECK(!sf_console_identity(p,&uid,&gid,&owner)&&uid==1000&&gid==1000&&owner==0x80000002u);
  CHECK(sf_console_authorize(p,"namespace-crash")==SF_DENIED);
  CHECK(!sf_console_logout(p,&ended)&&ended==2);
  CHECK(!sf_console_identity(p,&uid,&gid,&owner)&&!uid&&!gid&&owner==0x80000001u);
 }
 CHECK(sf_console_policy_fixture(p,0,0)==SF_INVALID&&!sf_session_count(sf_console_sessions(p)));
 CHECK(sf_console_authorize(p,"shutdown")==SF_DENIED);
 CHECK(sf_console_policy_init(p)==SF_NOT_FOUND&&!sf_session_count(sf_console_sessions(p))); /* no builtin in host fixture */
 printf("CONSOLE POLICY PASS: %u checks; private sessions, command gate, immutable admission identity and fail-closed initialization\n",checks);return 0;
}
