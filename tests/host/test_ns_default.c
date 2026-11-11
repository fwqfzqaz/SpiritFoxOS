#include <stdio.h>
#include "namespace.h"
/* Deliberately no private state header: exercise the service client boundary. */
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("NS DEFAULT FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 SfNamespace*n=sf_ns_default();CHECK(n!=0);sf_ns_init(n,0);CHECK(sf_ns_default()==n);
 CHECK(!sf_ns_create(n,"/scope",NODE_DIR,0));CHECK(!sf_ns_create(n,"/scope/file",NODE_FILE,0));
 int root=sf_ns_open(n,42,"/","/scope",OPEN_READ|OPEN_DIRECTORY);CHECK(root>0);
 int file=sf_ns_open(n,42,"/","/scope/file",OPEN_READ|OPEN_WRITE);CHECK(file>0);
 CHECK(sf_ns_write(n,42,file,"hello",5)==5);CHECK(sf_ns_seek(n,42,file,0)==0);
 char data[8]={0};CHECK(sf_ns_read(n,42,file,data,2)==2&&!memcmp(data,"he",2));
 CHECK(!sf_ns_handoff(n,42,file,43));CHECK(sf_ns_read(n,42,file,data,1)==SF_STALE_HANDLE);
 int duplicate=sf_ns_dup(n,43,file);CHECK(duplicate>0);
 CHECK(sf_ns_read(n,43,duplicate,data,3)==3&&!memcmp(data,"llo",3));CHECK(sf_ns_seek(n,43,file,0)==0);
 SfFileStat stat;CHECK(!sf_ns_stat(n,43,file,&stat)&&stat.size==5&&stat.offset==0);
 int relative=sf_ns_openat(n,42,root,"file",OPEN_READ);CHECK(relative>0);
 CHECK(sf_ns_read(n,42,relative,data,5)==5&&!memcmp(data,"hello",5));CHECK(sf_ns_default()==n);
 CHECK(!sf_ns_close(n,43,file));CHECK(!sf_ns_close(n,43,duplicate));
 CHECK(!sf_ns_close(n,42,relative));CHECK(!sf_ns_close(n,42,root));
 printf("Namespace opaque client tests passed: %u assertions\n",checks);return 0;
}
