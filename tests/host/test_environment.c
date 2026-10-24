#include <stdio.h>
#include "arguments.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("Environment FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static void boot(uint8_t*out,const SfArguments*args){
 memset(out,0,sizeof(SfBootArgs)+SF_ARGUMENT_BYTES);SfBootArgs*a=(void*)out;a->size=sizeof(*a)+args->bytes;a->abi_major=1;a->argc=args->count;a->envc=args->env_count;
 a->argv_offset=args->count?sizeof(*a):0;a->env_offset=args->env_count?sizeof(*a)+args->count*4:0;memcpy(out+sizeof(*a),args->data,args->bytes);
}
int main(void){
 SfArguments args,saved;union{SfBootArgs align;uint8_t data[sizeof(SfBootArgs)+SF_ARGUMENT_BYTES];} page;SfBootArgs*a=(void*)page.data;
 const char*argv[]={"argprobe.sfk","one"};const char*env[]={"MODE=test","EMPTY=","EQUAL=x=y=z","_A9=ok"};
 CHECK(!sf_invocation_pack(&args,argv,2,env,4));CHECK(!sf_arguments_validate(&args));boot(page.data,&args);CHECK(!sf_boot_arguments_validate(a,a->size));
 CHECK(!strcmp(sf_argument(a,a->size,0),argv[0]));CHECK(!strcmp(sf_environment(a,a->size,0),env[0]));CHECK(!strcmp(sf_environment_value(a,a->size,"MODE"),"test"));
 CHECK(sf_environment_value(a,a->size,"EMPTY")&&!*sf_environment_value(a,a->size,"EMPTY"));CHECK(!strcmp(sf_environment_value(a,a->size,"EQUAL"),"x=y=z"));CHECK(!strcmp(sf_environment_value(a,a->size,"_A9"),"ok"));
 for(unsigned i=0;i<4;i++)CHECK(!strcmp(sf_environment(a,a->size,i),env[i]));CHECK(!sf_environment(a,a->size,4));CHECK(!sf_environment_value(a,a->size,"mode"));CHECK(!sf_environment_value(a,a->size,"PATH"));
 CHECK(!sf_environment_value(a,a->size,""));CHECK(!sf_environment_value(a,a->size,"A=B"));CHECK(!sf_environment_value(a,a->size,0));CHECK(!sf_environment_name_valid(0,1));
 CHECK(!sf_invocation_pack(&args,0,0,env,4));CHECK(!sf_arguments_validate(&args));boot(page.data,&args);CHECK(!sf_boot_arguments_validate(a,a->size));CHECK(!sf_argument(a,a->size,0));CHECK(!strcmp(sf_environment_value(a,a->size,"MODE"),"test"));
 const char*invalid[]={"NO_EQUAL","=value","9BAD=x","BAD-NAME=x","BAD NAME=x","BAD.NAME=x","A\t=x"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){saved=args;CHECK(sf_invocation_pack(&args,argv,2,&invalid[i],1)==SF_INVALID);CHECK(!memcmp(&args,&saved,sizeof(args)));}
 const char*duplicate[]={"A=one","A=two"};CHECK(sf_invocation_pack(&args,argv,2,duplicate,2)==SF_INVALID);
 CHECK(sf_invocation_pack(&args,argv,2,env,9)==SF_NO_SPACE);CHECK(sf_invocation_pack(&args,argv,2,0,1)==SF_INVALID);
 const char*null[]={0};CHECK(sf_invocation_pack(&args,argv,2,null,1)==SF_INVALID);
 char long_name[67];memset(long_name,'A',sizeof(long_name));long_name[63]='=';long_name[64]='v';long_name[65]=0;const char*one[]={long_name};CHECK(!sf_invocation_pack(&args,argv,2,one,1));
 long_name[63]='A';long_name[64]='=';long_name[65]='v';long_name[66]=0;CHECK(sf_invocation_pack(&args,argv,2,one,1)==SF_INVALID);
 CHECK(!sf_invocation_pack(&args,argv,2,env,4));saved=args;args.env_count=9;CHECK(sf_arguments_validate(&args)==SF_BAD_FORMAT);args=saved;args.reserved=1;CHECK(sf_arguments_validate(&args)==SF_BAD_FORMAT);
 boot(page.data,&saved);a->env_offset--;CHECK(sf_boot_arguments_validate(a,a->size)==SF_BAD_FORMAT);boot(page.data,&saved);a->envc=9;CHECK(sf_boot_arguments_validate(a,a->size)==SF_BAD_FORMAT);
 /* Canonical byte transport is kernel mechanism; SDK also validates names. */
 args=saved;unsigned first=(unsigned)(args.data[2*4]|args.data[2*4+1]<<8)-sizeof(SfBootArgs);args.data[first]='9';CHECK(!sf_arguments_validate(&args));boot(page.data,&args);CHECK(sf_boot_arguments_validate(a,a->size)==SF_BAD_FORMAT);
 char maximum[509];maximum[0]='A';maximum[1]='=';memset(maximum+2,'x',505);maximum[507]=0;const char*maximum_env[]={maximum};
 CHECK(!sf_invocation_pack(&args,0,0,maximum_env,1)&&args.bytes==512);CHECK(!sf_arguments_validate(&args));boot(page.data,&args);CHECK(!sf_boot_arguments_validate(a,a->size));CHECK(strlen(sf_environment_value(a,a->size,"A"))==505);
 saved=args;maximum[507]='x';maximum[508]=0;CHECK(sf_invocation_pack(&args,0,0,maximum_env,1)==SF_NO_SPACE);CHECK(!memcmp(&args,&saved,sizeof(args)));
 unsigned seed=0x656e7631;
 for(unsigned round=0;round<1000;round++){
  char pairs[SF_ENVIRONMENT_MAX][32];const char*v[SF_ENVIRONMENT_MAX];seed=seed*1664525u+1013904223u;unsigned count=seed%9;
  for(unsigned i=0;i<count;i++){pairs[i][0]='A'+i;pairs[i][1]='=';unsigned n=(round+i)%25;for(unsigned j=0;j<n;j++)pairs[i][j+2]='a'+(i+j)%26;pairs[i][n+2]=0;v[i]=pairs[i];}
  CHECK(!sf_invocation_pack(&args,argv,2,v,count));CHECK(!sf_arguments_validate(&args));boot(page.data,&args);CHECK(!sf_boot_arguments_validate(a,a->size));
  for(unsigned i=0;i<count;i++){char key[2]={'A'+i,0};CHECK(!strcmp(sf_environment(a,a->size,i),v[i]));CHECK(!strcmp(sf_environment_value(a,a->size,key),v[i]+2));}
  CHECK(!sf_environment(a,a->size,count));
 }
 printf("ENVIRONMENT PASS: %u checks; explicit bounded names/values, empty/missing distinction and 1000 roundtrips\n",checks);return 0;
}
