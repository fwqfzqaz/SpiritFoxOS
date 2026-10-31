/* Host oracle tests for the bounded user-space SDK.
 * The host C library is used only as an independent reference: every SDK
 * function is compared against its libc counterpart on fixed and seeded
 * random inputs. No SDK code calls the host library. */
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdk.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"SDK FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static uint32_t seed=0x53464b31u;
static uint32_t next(void){seed=seed*1664525u+1013904223u;return seed;}
static unsigned pick(unsigned n){return(unsigned)(next()%n);}
static void random_text(char *out,unsigned max){
 unsigned n=pick(max+1);static const char alphabet[]="abcXYZ019 \t-+.,/\\\"'";
 for(unsigned i=0;i<n;i++)out[i]=alphabet[pick(sizeof(alphabet)-1)];
 out[n]=0;
}
static int host_v(char *buffer,size_t size,const char *format,...){
 va_list args;va_start(args,format);int result=vsnprintf(buffer,size,format,args);va_end(args);return result;
}
static int sdk_v(char *buffer,size_t size,const char *format,...){
 va_list args;va_start(args,format);int result=sf_vsnprintf(buffer,size,format,args);va_end(args);return result;
}
static int compare_int(const void *a,const void *b){
 int x=*(const int*)a,y=*(const int*)b;return x<y?-1:x>y?1:0;
}
/* C comparison functions specify the sign, not the magnitude, of a result. */
static int order(int value){return(value>0)-(value<0);}
static int valid_match(const int *values,size_t count,int key,const int *found){
 int present=0,matched=0;
 for(size_t i=0;i<count;i++){if(values[i]==key)present=1;if(found==values+i&&values[i]==key)matched=1;}
 return found?matched:!present;
}
#define SAME(fmt,...) do{checks++;int ra=snprintf(a,sizeof(a),fmt,__VA_ARGS__);int rb=sf_snprintf(b,sizeof(b),fmt,__VA_ARGS__);if(ra!=rb||strcmp(a,b)){fprintf(stderr,"SDK FMT line %d: %s (%d/%d)\n",__LINE__,fmt,ra,rb);return 1;}}while(0)
int main(void){
 /* All byte pairs, including NUL/high-bit bytes and zero-length comparisons. */
 for(unsigned x=0;x<256;x++)for(unsigned y=0;y<256;y++){
  char a[]={(char)x,0},b[]={(char)y,0};
  for(size_t n=0;n<=2;n++){
   int actual=order(sf_strncmp(a,b,n));
   CHECK(actual==order(strncmp(a,b,n))&&actual==(n?((x>y)-(x<y)):0));
  }
 }
 {
  const char a[]={'a',0,'x',0},b[]={'a',0,'y',0};
  CHECK(sf_strncmp(a,b,sizeof(a))==0);
  CHECK(sf_strncmp("ab","ac",1)==0);
  CHECK(order(sf_strncmp("ab","ac",2))==-1);
  CHECK(order(sf_strncmp("ac","ab",2))==1);
 }
 /* A save pointer may be NULL after the last token; never use it as a flag
  * to restart parsing. Empty delimiters still produce one complete token. */
 {
  static const struct {const char *text,*delim,*tokens[3];} cases[]={
   {"a,,b,",",",{"a","b",0}},{"abc","",{"abc",0,0}},
   {"",",",{0,0,0}},{",,,",",",{0,0,0}}
  };
  for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++){
   char a[16],b[16],*ca=0,*cb=0;strcpy(a,cases[i].text);strcpy(b,cases[i].text);
   for(unsigned j=0;j<3;j++){
    char *x=sf_strtok_r(j?0:a,cases[i].delim,&ca),*y=strtok_r(j?0:b,cases[i].delim,&cb);
    const char *expected=cases[i].tokens[j];
    CHECK(expected?(x&&y&&!strcmp(x,expected)&&!strcmp(y,expected)):(!x&&!y));
    if(!expected)break;
   }
  }
 }
 /* String and memory helpers against the host implementations. */
 for(unsigned round=0;round<200;round++){
  char source[72],stage[96],host[96],needle[8],accept[8];
  random_text(source,64);random_text(needle,4);random_text(accept,4);
  memset(stage,0,sizeof(stage));memset(host,0,sizeof(host));memcpy(stage,source,70);memcpy(host,source,70);
  size_t bytes=pick(60),shift=pick(6);
  sf_memmove(stage+shift,stage,bytes);memmove(host+shift,host,bytes);
  CHECK(!memcmp(stage,host,sizeof(stage)));
  for(int c=0;c<256;c++){
   const void*x=sf_memchr(source,c,strlen(source)),*y=memchr(source,c,strlen(source));
   CHECK((x==0)==(y==0)&&(!x||(size_t)((const char*)x-source)==(size_t)((const char*)y-source)));
  }
  for(unsigned n=0;n<=strlen(source);n++)CHECK(order(sf_strncmp(source,source,n))==order(strncmp(source,source,n))&&order(sf_strncmp(source,needle,n))==order(strncmp(source,needle,n)));
  char a1[80],b1[80];memset(a1,0,sizeof(a1));memset(b1,0,sizeof(b1));
  CHECK(!strcmp(sf_strcpy(a1,source),strcpy(b1,source)));
  for(unsigned n=0;n<16;n++){memset(a1,'#',sizeof(a1));memset(b1,'#',sizeof(b1));sf_strncpy(a1,source,n);strncpy(b1,source,n);CHECK(!memcmp(a1,b1,sizeof(a1)));}
  memset(a1,0,sizeof(a1));memset(b1,0,sizeof(b1));strcpy(a1,source);strcpy(b1,source);
  CHECK(!strcmp(sf_strcat(a1,needle),strcat(b1,needle)));
  memset(a1,0,sizeof(a1));memset(b1,0,sizeof(b1));strcpy(a1,source);strcpy(b1,source);
  CHECK(!strcmp(sf_strncat(a1,needle,2),strncat(b1,needle,2)));
  for(int c=0;c<256;c++){
   char*x=sf_strchr(source,c),*y=strchr(source,c);CHECK((x==0)==(y==0)&&(!x||(size_t)(x-source)==(size_t)(y-source)));
   x=sf_strrchr(source,c);y=strrchr(source,c);CHECK((x==0)==(y==0)&&(!x||(size_t)(x-source)==(size_t)(y-source)));
  }
  {
   char*x=sf_strstr(source,needle),*y=strstr(source,needle);
   CHECK((x==0)==(y==0)&&(!x||(size_t)(x-source)==(size_t)(y-source)));
  }
  CHECK(sf_strspn(source,accept)==strspn(source,accept)&&sf_strcspn(source,accept)==strcspn(source,accept));
  {
   char*x=sf_strpbrk(source,accept),*y=strpbrk(source,accept);
   CHECK((x==0)==(y==0)&&(!x||(size_t)(x-source)==(size_t)(y-source)));
  }
  char t1[80],t2[80],*c1=0,*c2=0;strcpy(t1,source);strcpy(t2,source);
  /* Only the first call receives a string. The cursor's final value is opaque. */
  char*x=sf_strtok_r(t1,accept,&c1),*y=strtok_r(t2,accept,&c2);
  for(;;){CHECK(x==0?(y==0):(y!=0&&!strcmp(x,y)));if(!x)break;x=sf_strtok_r(0,accept,&c1);y=strtok_r(0,accept,&c2);}
 }
 /* Character classification over the complete byte range. */
 for(int c=-256;c<512;c++){
  int value=(unsigned char)c;
  CHECK(!!sf_isalpha(value)==!!isalpha(value)&&!!sf_isdigit(value)==!!isdigit(value)&&!!sf_isalnum(value)==!!isalnum(value));
  CHECK(!!sf_isspace(value)==!!isspace(value)&&!!sf_isupper(value)==!!isupper(value)&&!!sf_islower(value)==!!islower(value));
  CHECK(!!sf_isprint(value)==!!isprint(value)&&!!sf_ispunct(value)==!!ispunct(value)&&!!sf_isxdigit(value)==!!isxdigit(value));
  CHECK(!!sf_isgraph(value)==!!isgraph(value)&&!!sf_iscntrl(value)==!!iscntrl(value));
  if(c>=0&&c<256)CHECK(sf_toupper(c)==toupper(value)&&sf_tolower(c)==tolower(value));
 }
 CHECK(sf_abs(-7)==7&&sf_abs(INT_MAX)==INT_MAX&&sf_abs(INT_MIN)==INT_MIN);
 CHECK(sf_labs(LONG_MIN)==LONG_MIN&&sf_labs(-1)==1);
 /* Numeric conversion, including canonical overflow clamping. */
 {
  /* Some Windows CRTs consume no digits for incomplete hex prefixes. The SDK
   * keeps C's longest valid subject sequence (the leading zero), tested here
   * against explicit values/offsets rather than that host dialect. */
  static const struct {const char *text;size_t end;} prefixes[]={
   {"0x",1},{"0X",1},{"0xg",1},{"0Xg",1},{"+0x",2},{"-0x",2},{" \t+0Xg",4}
  };
  for(unsigned i=0;i<sizeof(prefixes)/sizeof(*prefixes);i++)for(int base=0;base<=16;base+=16){
   char *end;const char *text=prefixes[i].text;
   CHECK(sf_strtol(text,&end,base)==0&&(size_t)(end-text)==prefixes[i].end);
   CHECK(sf_strtoul(text,&end,base)==0&&(size_t)(end-text)==prefixes[i].end);
  }
  static const char*cases[]={"","0","1","-1","+1","  42","\t-7","0x1f","0X1F","0x1g","077","08","zz","Z","-zz","0x0","  +","--1","9223372036854775807","9223372036854775808","-9223372036854775808","-9223372036854775809","18446744073709551615","18446744073709551616","0xffffffffffffffff","0x10000000000000000"};
  static const int bases[]={0,2,8,10,16,36,0,10,0,16,0,0,36,36,36,16,10,10,10,10,10,10,10,10,16,16};
  for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++){
   char *ea,*eb;long va=sf_strtol(cases[i],&ea,bases[i]);long vb=strtol(cases[i],&eb,bases[i]);
   if(va!=vb||ea-cases[i]!=eb-cases[i])fprintf(stderr,"SDK strtol case '%s' base %d: %ld/%ld, end %zu/%zu\n",cases[i],bases[i],va,vb,(size_t)(ea-cases[i]),(size_t)(eb-cases[i]));
   CHECK(va==vb&&(size_t)(ea-cases[i])==(size_t)(eb-cases[i]));
   if(cases[i][0]!='-'){
    char *ua,*ub;unsigned long wa=sf_strtoul(cases[i],&ua,bases[i]),wb=strtoul(cases[i],&ub,bases[i]);
    CHECK(wa==wb&&(size_t)(ua-cases[i])==(size_t)(ub-cases[i]));
   }
  }
  const char*invalid="123";char*end=0;
  CHECK(sf_strtol(invalid,&end,1)==0&&end==invalid);
  CHECK(sf_strtol(invalid,&end,37)==0&&end==invalid);
  CHECK(sf_atoi(" -42x")==-42);
  /* long is 32-bit on LLP64 hosts; the SDK clamps to that host's LONG_MAX. */
 #if LONG_MAX > 2147483647L
  CHECK(sf_atol("2147483648")==2147483648L);
 #else
  CHECK(sf_atol("2147483648")==LONG_MAX);
 #endif
 }
 for(unsigned round=0;round<1000;round++){
  char text[24];unsigned n=pick(16);static const char alphabet[]="0123456789abcdefABCDEFxX+- \tgz";
  for(unsigned i=0;i<n;i++)text[i]=alphabet[pick(sizeof(alphabet)-1)];text[n]=0;
  static const int bases[]={0,2,8,10,16,36};int base=bases[pick(6)];
  char*ea,*eb;long va=sf_strtol(text,&ea,base);long vb=strtol(text,&eb,base);
  CHECK(va==vb&&(size_t)(ea-text)==(size_t)(eb-text));
  unsigned long wa=sf_strtoul(text,&ea,base),wb=strtoul(text,&eb,base);
  CHECK(wa==wb&&(size_t)(ea-text)==(size_t)(eb-text));
 }
 /* Fixed formatting cases. */
 {
  char a[96],b[96];
  SAME("%d",7);SAME("%d",-7);SAME("%5d",7);SAME("%-5d",7);SAME("%05d",7);SAME("%05d",-7);
  SAME("%+d",7);SAME("% d",7);SAME("%+05d",7);SAME("%8.3d",7);SAME("%8.3d",-7);SAME("%.0d",0);SAME("%.5d",0);
  SAME("%ld",1234567890L);SAME("%lld",-123456789012345LL);SAME("%hd",-32768);SAME("%hhd",-128);SAME("%hhd",127);
  SAME("%u",42u);SAME("%5u",42u);SAME("%-5u",42u);SAME("%05u",42u);SAME("%x",0x2a);SAME("%X",0x2a);
  SAME("%08x",0x2a);SAME("%o",42u);SAME("%.5u",42u);SAME("%.0u",0u);SAME("%llx",0x123456789abcdefULL);
  SAME("%hhx",0xf0u);SAME("%hx",0xffffu);
  /* UCRT renders %p without the 0x prefix, so assert the SDK's own form. */
  CHECK(sf_snprintf(b,sizeof(b),"%p",(void*)0x1234)==6&&!strcmp(b,"0x1234"));
  CHECK(sf_snprintf(b,sizeof(b),"%12p",(void*)0x1234)==12&&!strcmp(b,"      0x1234"));
  SAME("%s","hello");SAME("%10s","hello");SAME("%-10s","hello");SAME("%.3s","hello");SAME("%10.3s","hello");
  SAME("%.0s","hello");SAME("%3s","ab");SAME("%c",'A');SAME("%5c",'A');SAME("%-5c",'A');
  SAME("%s/%d/%08x","abc",-3,0x11u);SAME("[%.4s]","abcdefg");
  CHECK(sf_snprintf(a,sizeof(a),"%s","")==0&&a[0]==0);
  CHECK(sf_snprintf(a,sizeof(a),"%%")==1&&a[0]=='%'&&a[1]==0);
  CHECK(sf_snprintf(a,0,"%s","x")==1);
  CHECK(sf_snprintf(a,1,"%s","x")==1&&a[0]==0);
 }
 /* Seeded formatting oracle; host vsnprintf is the reference. */
 {
  static const char*int_flags[]={"","-","0","+"," ","-0","+0","0+"};
  static const char*string_flags[]={"","-"};
  static const char*int_lengths[]={"","l","ll","h","hh"};
  static const char*unsigned_conversions[]={"u","o","x","X"};
  for(unsigned round=0;round<700;round++){
   char format[24];unsigned n=0;format[n++]='%';
   unsigned group=pick(5);
   if(group==0||group==1){
    const char*flags=int_flags[pick(sizeof(int_flags)/sizeof(*int_flags))];
    for(unsigned i=0;flags[i];i++)format[n++]=flags[i];
    unsigned width=pick(13);if(width){if(width>9)format[n++]='1';format[n++]=(char)('0'+width%10);}
    if(pick(2)){format[n++]='.';unsigned precision=pick(13);if(precision>9)format[n++]='1';format[n++]=(char)('0'+precision%10);}
    unsigned li=pick(5);const char*length=int_lengths[li];for(unsigned i=0;length[i];i++)format[n++]=length[i];
    format[n++]=group==0?'d':'i';format[n]=0;
    char a[96],b[96];int ra,rb;
    if(li==1){long value=(long)next()*(pick(2)?1:-1);ra=snprintf(a,sizeof(a),format,value);rb=sf_snprintf(b,sizeof(b),format,value);}
    else if(li==2){long long value=(long long)next()*(pick(2)?1:-1);ra=snprintf(a,sizeof(a),format,value);rb=sf_snprintf(b,sizeof(b),format,value);}
    else {int value=(int)next()^(pick(3)?0:(int)0x80000000);ra=snprintf(a,sizeof(a),format,value);rb=sf_snprintf(b,sizeof(b),format,value);}
    CHECK(ra==rb&&!strcmp(a,b));
   }else if(group==2||group==3){
    const char*flags=int_flags[pick(sizeof(int_flags)/sizeof(*int_flags))];
    for(unsigned i=0;flags[i];i++)format[n++]=flags[i];
    unsigned width=pick(13);if(width){if(width>9)format[n++]='1';format[n++]=(char)('0'+width%10);}
    if(pick(2)){format[n++]='.';unsigned precision=pick(13);if(precision>9)format[n++]='1';format[n++]=(char)('0'+precision%10);}
    unsigned li=pick(5);const char*length=int_lengths[li];for(unsigned i=0;length[i];i++)format[n++]=length[i];
    format[n++]=unsigned_conversions[pick(4)][0];format[n]=0;
    char a[96],b[96];int ra,rb;
    if(li==1){unsigned long value=(unsigned long)next()*next();ra=snprintf(a,sizeof(a),format,value);rb=sf_snprintf(b,sizeof(b),format,value);}
    else if(li==2){unsigned long long value=(unsigned long long)next()*next();ra=snprintf(a,sizeof(a),format,value);rb=sf_snprintf(b,sizeof(b),format,value);}
    else {unsigned value=(unsigned)next()*next();ra=snprintf(a,sizeof(a),format,value);rb=sf_snprintf(b,sizeof(b),format,value);}
    CHECK(ra==rb&&!strcmp(a,b));
   }else{
    const char*flags=string_flags[pick(2)];
    for(unsigned i=0;flags[i];i++)format[n++]=flags[i];
    unsigned width=pick(13);if(width){if(width>9)format[n++]='1';format[n++]=(char)('0'+width%10);}
    if(pick(2)){format[n++]='.';unsigned precision=pick(10);format[n++]=(char)('0'+precision);}
    format[n++]='s';format[n]=0;
    char text[24],a[96],b[96];random_text(text,16);
    int ra=snprintf(a,sizeof(a),format,text),rb=sf_snprintf(b,sizeof(b),format,text);
    CHECK(ra==rb&&!strcmp(a,b));
   }
  }
 }
 /* Truncation: return value counts the full text, buffer bytes match host. */
 for(unsigned size=0;size<=40;size++){
  char host[48],sdk[48],text[8];memset(host,0x5a,sizeof(host));memset(sdk,0x5a,sizeof(sdk));
  text[0]='s';text[1]='d';text[2]='k';text[3]=(char)('0'+(size%10));text[4]=0;
  int a=host_v(host,size,"%s/%d/%08x",text,(int)(size*7),0xabcdu);
  int b=sdk_v(sdk,size,"%s/%d/%08x",text,(int)(size*7),0xabcdu);
  CHECK(a==b&&!memcmp(host,sdk,sizeof(host)));
 }
 /* Either library may return any matching duplicate; verify membership and
  * value independently instead of requiring identical array offsets. */
 {
  const int values[]={-3,-3,0,0,0,7,7};
  for(size_t n=0;n<=sizeof(values)/sizeof(*values);n++)for(int key=-4;key<=8;key++){
   const int *pa=sf_bsearch(&key,values,n,sizeof(int),compare_int);
   const int *pb=bsearch(&key,values,n,sizeof(int),compare_int);
   CHECK(valid_match(values,n,key,pa)&&valid_match(values,n,key,pb));
  }
 }
 /* Seeded binary search over sorted arrays, including duplicate keys. */
 for(unsigned round=0;round<100;round++){
  int values[32];unsigned n=pick(31)+1;
  for(unsigned i=0;i<n;i++)values[i]=(int)(next()%200)-100;
  for(unsigned i=1;i<n;i++)for(unsigned j=i;j&&values[j-1]>values[j];j--){int t=values[j];values[j]=values[j-1];values[j-1]=t;}
  int key=(int)(next()%220)-110;
  const int*pa=sf_bsearch(&key,values,n,sizeof(int),compare_int);
  const int*pb=bsearch(&key,values,n,sizeof(int),compare_int);
  CHECK(valid_match(values,n,key,pa)&&valid_match(values,n,key,pb));
 }
 fprintf(stdout,"SDK PASS %u\n",checks);
 return 0;
}
