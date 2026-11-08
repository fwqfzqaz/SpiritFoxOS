#include <stdio.h>
#include "accounts.h"
static SfAccountStore store;
#define FIXTURE "root:0:0:f18371049203aa9e442c640df9630307:33823fd26bea823886981dc012a75be1a924f1caa1ac5e4e0fca5bd9d9b56b86\n" \
 "alice:1000:1000:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n" \
 "bob:1001:1001:b1b5734266f3e90733bc17d2d50a544f:1002a488c324bfa22af6275aa76941c322dff1b36861fc97134ffaf2ff1c1f1e\n"
#define CHECK(x) do{checks++;if(!(x)){printf("ACCOUNTS FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int rejects(const char*text){SfAccountStore s;return sf_accounts_parse(text,strlen(text),&s)==SF_BAD_FORMAT;}
int main(void){
 unsigned checks=0;const SfAccount*a=0;
 CHECK(!sf_accounts_parse(FIXTURE,strlen(FIXTURE),&store));CHECK(store.count==3);
 CHECK(!sf_account_lookup(&store,"alice",&a)&&a->uid==1000&&a->gid==1000&&!strcmp(a->name,"alice"));
 CHECK(sf_account_lookup(&store,"Alice",&a)==SF_NOT_FOUND);CHECK(sf_account_lookup(&store,"",&a)==SF_NOT_FOUND);
 CHECK(sf_account_lookup(0,"root",&a)==SF_INVALID);CHECK(sf_account_lookup(&store,0,&a)==SF_INVALID);
 CHECK(!sf_account_by_uid(&store,1001,&a)&&!strcmp(a->name,"bob"));CHECK(sf_account_by_uid(&store,42,&a)==SF_NOT_FOUND);
 char home[SF_ACCOUNT_HOME_MAX+1];CHECK(!sf_account_home(a,home,sizeof(home))&&!strcmp(home,"/home/bob"));CHECK(sf_account_home(a,home,4)==SF_INVALID);CHECK(sf_account_home(0,home,sizeof(home))==SF_INVALID);
 CHECK(!sf_account_verify(&store.accounts[1],"alice"));CHECK(sf_account_verify(&store.accounts[1],"Alice")==SF_DENIED);
 CHECK(sf_account_verify(&store.accounts[1],"")==SF_DENIED);CHECK(sf_account_verify(&store.accounts[1],0)==SF_INVALID);
 char long_password[40];memset(long_password,'x',sizeof(long_password));long_password[33]=0;CHECK(sf_account_verify(&store.accounts[1],long_password)==SF_DENIED);
 SfAccount tampered=store.accounts[1];tampered.hash[0]^=1;CHECK(sf_account_verify(&tampered,"alice")==SF_DENIED);
 /* 注释与空行被忽略，数据行前后空白不被接受。 */
 CHECK(!sf_accounts_parse("# comment\n\n" FIXTURE,strlen("# comment\n\n" FIXTURE),&store));
 /* 拒绝：缺少 root、重复名称/uid、名称越界、非法 uid、摘要长度/十六进制、额外字段、CR、超过 8 条。 */
 CHECK(rejects("alice:1000:1000:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"));
 CHECK(rejects(FIXTURE "alice:1002:1002:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"));
 CHECK(rejects(FIXTURE "carol:1000:1002:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"));
 CHECK(rejects("1alice:1000:1000:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"));
 CHECK(rejects("abcdefghi:1000:1000:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"));
 CHECK(rejects("root:65536:0:f18371049203aa9e442c640df9630307:33823fd26bea823886981dc012a75be1a924f1caa1ac5e4e0fca5bd9d9b56b86\n"));
 CHECK(rejects("root:0:0:f18371049203aa9e442c640df9630307:33823fd26bea823886981dc012a75be1a924f1caa1ac5e4e0fca5bd9d9b56b8\n"));
 CHECK(rejects("root:0:0:f18371049203aa9e442c640df9630307:33823fd26bea823886981dc012a75be1a924f1caa1ac5e4e0fca5bd9d9b56b8g\n"));
 CHECK(rejects("root:0:0:f18371049203aa9e442c640df9630307:33823fd26bea823886981dc012a75be1a924f1caa1ac5e4e0fca5bd9d9b56b86:extra\n"));
 CHECK(rejects("root:0:0:f18371049203aa9e442c640df9630307:33823fd26bea823886981dc012a75be1a924f1caa1ac5e4e0fca5bd9d9b56b86\r\n"));
 CHECK(rejects(""));
 CHECK(rejects("# only comments\n"));
 {SfAccountStore s;CHECK(sf_accounts_parse(FIXTURE "u1:100:1:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"
  "u2:101:1:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"
  "u3:102:1:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"
  "u4:103:1:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"
  "u5:104:1:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"
  "u6:105:1:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n",strlen(FIXTURE "u1:100:1:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"
  "u2:101:1:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"
  "u3:102:1:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"
  "u4:103:1:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"
  "u5:104:1:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"
  "u6:105:1:2dc35541b1271e2892045008ec2934cb:a54941a5981de385f5a4a92e73db1e28b573b3241442421bb610cd16256c561d\n"),&s)==SF_NO_SPACE);}
 CHECK(sf_accounts_parse(FIXTURE,strlen(FIXTURE),0)==SF_INVALID);
 printf("ACCOUNTS PASS: %u checks; parse, uniqueness, root invariant, salted digest\n",checks);return 0;
}
