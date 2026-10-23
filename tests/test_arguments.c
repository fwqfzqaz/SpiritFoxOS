#include <stdio.h>
#include "arguments.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("Arguments FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static void boot(uint8_t*out,const SfArguments*args){
 memset(out,0,sizeof(SfBootArgs)+SF_ARGUMENT_BYTES);SfBootArgs*a=(void*)out;a->size=sizeof(*a)+args->bytes;a->abi_major=1;a->argc=args->count;a->argv_offset=args->count?sizeof(*a):0;memcpy(out+sizeof(*a),args->data,args->bytes);
}
int main(void){
 SfArguments args,copy;const char*values[]={"argprobe.sfk","alpha beta",""};
 union{SfBootArgs align;uint8_t data[sizeof(SfBootArgs)+SF_ARGUMENT_BYTES];} page;
 CHECK(!sf_arguments_pack(&args,values,3));CHECK(!sf_arguments_validate(&args));boot(page.data,&args);SfBootArgs*a=(void*)page.data;
 CHECK(!sf_boot_arguments_validate(a,a->size));for(unsigned i=0;i<3;i++)CHECK(!strcmp(sf_argument(a,a->size,i),values[i]));CHECK(!sf_argument(a,a->size,3));
 CHECK(sf_boot_arguments_validate(a,sizeof(*a)-1)==SF_BAD_FORMAT);CHECK(sf_boot_arguments_validate(a,sizeof(page)+1)==SF_BAD_FORMAT);CHECK(sf_boot_arguments_validate(0,sizeof(*a))==SF_BAD_FORMAT);
 CHECK(!sf_arguments_pack(&args,0,0));CHECK(!sf_arguments_validate(&args));boot(page.data,&args);CHECK(!sf_boot_arguments_validate(a,sizeof(*a)));CHECK(!sf_argument(a,sizeof(*a),0));
 copy=args;CHECK(sf_arguments_pack(&args,0,1)==SF_INVALID&&!memcmp(&args,&copy,sizeof(args)));CHECK(sf_arguments_pack(&args,values,SF_ARGUMENT_MAX+1)==SF_NO_SPACE);
 CHECK(sf_arguments_validate(0)==SF_INVALID);const char*null[]={0};CHECK(sf_arguments_pack(&args,null,1)==SF_INVALID);
 char large[SF_ARGUMENT_BYTES+1];memset(large,'x',sizeof(large));large[507]=0;const char*one[]={large};CHECK(!sf_arguments_pack(&args,one,1)&&args.bytes==512);CHECK(!sf_arguments_validate(&args));
 boot(page.data,&args);CHECK(!sf_boot_arguments_validate(a,sizeof(page)));CHECK(strlen(sf_argument(a,sizeof(page),0))==507);
 large[507]='x';large[508]=0;copy=args;CHECK(sf_arguments_pack(&args,one,1)==SF_NO_SPACE&&!memcmp(&args,&copy,sizeof(args)));
 CHECK(!sf_arguments_pack(&args,values,3));
 for(unsigned bit=0;bit<32;bit++){copy=args;copy.count^=1u<<bit;CHECK(sf_arguments_validate(&copy)==SF_BAD_FORMAT);copy=args;copy.bytes^=1u<<bit;CHECK(sf_arguments_validate(&copy)==SF_BAD_FORMAT);}
 for(unsigned i=0;i<args.count*4;i++){copy=args;copy.data[i]^=1;CHECK(sf_arguments_validate(&copy)==SF_BAD_FORMAT);}
 for(unsigned i=args.count*4;i<args.bytes;i++)if(!args.data[i]){copy=args;copy.data[i]=1;CHECK(sf_arguments_validate(&copy)==SF_BAD_FORMAT);}
 boot(page.data,&args);a->argv_offset++;CHECK(sf_boot_arguments_validate(a,a->size)==SF_BAD_FORMAT);boot(page.data,&args);a->envc=1;CHECK(sf_boot_arguments_validate(a,a->size)==SF_BAD_FORMAT);
 boot(page.data,&args);a->env_offset=sizeof(*a);CHECK(sf_boot_arguments_validate(a,a->size)==SF_BAD_FORMAT);boot(page.data,&args);a->size--;CHECK(sf_boot_arguments_validate(a,sizeof(*a)+args.bytes)==SF_ABI_MISMATCH);
 boot(page.data,&args);a->abi_major=2;CHECK(sf_boot_arguments_validate(a,a->size)==SF_ABI_MISMATCH);boot(page.data,&args);a->abi_minor=1;CHECK(sf_boot_arguments_validate(a,a->size)==SF_ABI_MISMATCH);
 const struct {const char*line;int count;const char*words[5];} cases[]={
  {"",0,{0}},{" \t ",0,{0}},{"app plain",2,{"app","plain"}},{"app \"alpha beta\" ''",3,{"app","alpha beta",""}},
  {"app a\\ b 'c\\d'",3,{"app","a b","c\\d"}},{"app ab\"cd\"ef",2,{"app","abcdef"}},
  {"app \"a\\\"b\"",2,{"app","a\"b"}},{"app \"unterminated",SF_INVALID,{0}},{"app 'unterminated",SF_INVALID,{0}},{"app tail\\",SF_INVALID,{0}},
  {"app $literal ;name",3,{"app","$literal",";name"}}
 };
 for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++){char line[128],*words[SF_ARGUMENT_MAX];memcpy(line,cases[i].line,strlen(cases[i].line)+1);int n=sf_arguments_split(line,words,SF_ARGUMENT_MAX);CHECK(n==cases[i].count);for(int j=0;j<n;j++)CHECK(!strcmp(words[j],cases[i].words[j]));}
 char line[]="a b c d e f g h i";char*words[SF_ARGUMENT_MAX+1];CHECK(sf_arguments_split(line,words,8)==SF_NO_SPACE);CHECK(sf_arguments_split(line,words,0)==SF_INVALID);
 unsigned seed=0x6a726773;
 for(unsigned round=0;round<1000;round++){char strings[SF_ARGUMENT_MAX][48];const char*v[SF_ARGUMENT_MAX];seed=seed*1664525u+1013904223u;unsigned count=seed%9;
  for(unsigned i=0;i<count;i++){seed=seed*1664525u+1013904223u;unsigned n=seed%48;for(unsigned j=0;j<n;j++)strings[i][j]='a'+(i+j+round)%26;strings[i][n]=0;v[i]=strings[i];}
  CHECK(!sf_arguments_pack(&args,v,count));CHECK(!sf_arguments_validate(&args));boot(page.data,&args);CHECK(!sf_boot_arguments_validate(a,a->size));for(unsigned i=0;i<count;i++)CHECK(!strcmp(sf_argument(a,a->size,i),v[i]));CHECK(!sf_argument(a,a->size,count));
 }
 printf("ARGUMENTS PASS: %u checks; bounded canonical payload, lexer, 1000 roundtrips and malformed offsets\n",checks);return 0;
}
