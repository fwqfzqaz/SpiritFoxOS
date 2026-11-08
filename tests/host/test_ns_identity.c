#include <stdio.h>
#include "namespace.h"
static SfNamespace ns;static unsigned checks;
static long console(const void*p,size_t n){(void)p;return (long)n;}
#define CHECK(x) do{checks++;if(!(x)){printf("NS IDENTITY FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define OPEN(owner,uid,gid,path,flags) sf_ns_open_as(&ns,owner,"/",path,flags,uid,gid)
int main(void){
 sf_ns_init(&ns,console);
 CHECK(!sf_ns_create(&ns,"/home",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/tmp",NODE_DIR,0));
 CHECK(!sf_ns_assign(&ns,"/tmp",0,0,0777));
 CHECK(!sf_ns_create(&ns,"/home/alice",NODE_DIR,0)&&!sf_ns_assign(&ns,"/home/alice",1000,1000,0700));
 CHECK(!sf_ns_create(&ns,"/home/bob",NODE_DIR,0)&&!sf_ns_assign(&ns,"/home/bob",1001,1001,0700));
 CHECK(!sf_ns_create(&ns,"/home/alice/note",NODE_FILE,0)&&!sf_ns_assign(&ns,"/home/alice/note",1000,1000,0600));
 CHECK(!sf_ns_create(&ns,"/home/alice/open",NODE_FILE,0)&&!sf_ns_assign(&ns,"/home/alice/open",1000,1000,0644));
 CHECK(!sf_ns_create(&ns,"/home/bob/secret",NODE_FILE,0)&&!sf_ns_assign(&ns,"/home/bob/secret",1001,1001,0600));
 CHECK(!sf_ns_create(&ns,"/tmp/shared",NODE_FILE,0)&&!sf_ns_assign(&ns,"/tmp/shared",0,0,0644));
 CHECK(!sf_ns_create(&ns,"/tmp/group",NODE_FILE,0)&&!sf_ns_assign(&ns,"/tmp/group",0,1000,0640));
 int h;
 /* 属主权限：alice 可读写自己的文件，bob 因父目录 0700 无法穿越。 */
 h=OPEN(42,1000,1000,"/home/alice/note",OPEN_READ);CHECK(h>0);CHECK(!sf_ns_close(&ns,42,h));
 CHECK(OPEN(43,1001,1001,"/home/alice/note",OPEN_READ)==SF_DENIED);
 CHECK(OPEN(43,1001,1001,"/home/alice",OPEN_READ|OPEN_DIRECTORY)==SF_DENIED);
 CHECK(OPEN(43,1001,1001,"/home/alice",OPEN_READ)==SF_DENIED);
 /* root 绕过；alice 不能用只读打开写。 */
 h=OPEN(1,0,0,"/home/bob/secret",OPEN_READ);CHECK(h>0);CHECK(!sf_ns_close(&ns,1,h));
 CHECK(OPEN(42,1000,1000,"/tmp/shared",OPEN_READ)>0);
 CHECK(OPEN(42,1000,1000,"/tmp/shared",OPEN_WRITE)==SF_DENIED);
 /* 组权限：/tmp/group 属主 root、组 1000，组员 alice 可读，bob 不可。 */
 h=OPEN(42,1000,1000,"/tmp/group",OPEN_READ);CHECK(h>0);CHECK(!sf_ns_close(&ns,42,h));
 CHECK(OPEN(43,1001,1001,"/tmp/group",OPEN_READ)==SF_DENIED);
 /* 创建与删除要求父目录写+执行，跨用户目录被拒绝。 */
 CHECK(!sf_ns_create_owned(&ns,"/tmp/alice-file",NODE_FILE,0,1000,1000,0644));
 CHECK(!sf_ns_create_owned(&ns,"/home/alice/new",NODE_FILE,0,1000,1000,0644));
 CHECK(sf_ns_create_owned(&ns,"/home/bob/evil",NODE_FILE,0,1000,1000,0644)==SF_DENIED);
 CHECK(sf_ns_create_owned(&ns,"/home/mallory",NODE_DIR,0,1000,1000,0755)==SF_DENIED);
 CHECK(sf_ns_create_owned(&ns,"/tmp/x",NODE_FILE,0,1000,1000,01777)==SF_INVALID);
 /* 相对删除：alice 可删自己的文件，bob 不能删 alice 家目录中的文件。 */
 h=OPEN(42,1000,1000,"/home/alice",OPEN_WRITE|OPEN_DIRECTORY);CHECK(h>0);
 CHECK(sf_ns_unlink_as(&ns,42,h,"open",1000,1000)==0);CHECK(!sf_ns_close(&ns,42,h));
 h=OPEN(43,1001,1001,"/tmp",OPEN_WRITE|OPEN_DIRECTORY);CHECK(h>0);
 /* 无 sticky 位：0777 目录内允许删除他人文件；嵌套目录仍需逐级执行权限。 */
 CHECK(sf_ns_unlink_as(&ns,43,h,"alice-file",1001,1001)==0);
 CHECK(!sf_ns_create_owned(&ns,"/tmp/alice-private",NODE_DIR,0,1000,1000,0700));
 CHECK(!sf_ns_create_owned(&ns,"/tmp/alice-private/secret",NODE_FILE,0,1000,1000,0644));
 CHECK(sf_ns_unlink_as(&ns,43,h,"alice-private/secret",1001,1001)==SF_DENIED);
 CHECK(!sf_ns_close(&ns,43,h));
 /* 深层穿越与非目录祖先。 */
 CHECK(!sf_ns_create(&ns,"/home/alice/private",NODE_DIR,0)&&!sf_ns_assign(&ns,"/home/alice/private",1000,1000,0700));
 CHECK(!sf_ns_create(&ns,"/home/alice/private/file",NODE_FILE,0)&&!sf_ns_assign(&ns,"/home/alice/private/file",0,0,0644));
 h=OPEN(42,1000,1000,"/home/alice/private/file",OPEN_READ);CHECK(h>0);CHECK(!sf_ns_close(&ns,42,h));
 CHECK(OPEN(43,1001,1001,"/home/alice/private/file",OPEN_READ)==SF_DENIED);
 CHECK(OPEN(42,1000,1000,"/home/alice/note/x",OPEN_READ)==SF_NOT_FOUND);
 printf("NS IDENTITY PASS: %u checks; owner/group/other bits, traversal, create/unlink guards\n",checks);return 0;
}
