/* Bounded user-space SDK helpers, adapted from the pre-refactor libc.
 * Pure functions only: no syscalls, no allocation, no shared mutable state.
 * Lengths and widths are clamped; formatting always terminates within size. */
#include "sdk.h"
#include <stdarg.h>
#define SF_LONG_MAX ((long)(~0ul>>1))
#define SF_LONG_MIN (-SF_LONG_MAX-1)
#define SF_STRLEN_MAX 4096u

void *sf_memmove(void *dst,const void *src,size_t bytes){
 uint8_t*d=dst;const uint8_t*s=src;
 if(bytes&&d!=s){
  if((uintptr_t)d<(uintptr_t)s){for(size_t i=0;i<bytes;i++)d[i]=s[i];}
  else for(size_t i=bytes;i;i--)d[i-1]=s[i-1];
 }
 return dst;
}
void *sf_memchr(const void *data,int c,size_t bytes){
 const uint8_t*p=data;for(size_t i=0;i<bytes;i++)if(p[i]==(uint8_t)c)return(void*)(p+i);return 0;
}
int sf_strncmp(const char*a,const char*b,size_t bytes){
 for(size_t i=0;i<bytes;i++){if(a[i]!=b[i])return(int)(unsigned char)a[i]-(int)(unsigned char)b[i];if(!a[i])return 0;}
 return 0;
}
char *sf_strcpy(char *dst,const char *src){size_t i=0;while((dst[i]=src[i]))i++;return dst;}
char *sf_strncpy(char *dst,const char *src,size_t bytes){
 size_t i=0;for(;i<bytes&&src[i];i++)dst[i]=src[i];for(;i<bytes;i++)dst[i]=0;return dst;
}
char *sf_strcat(char *dst,const char *src){size_t i=0;while(dst[i])i++;size_t j=0;while((dst[i++]=src[j++])){}return dst;}
char *sf_strncat(char *dst,const char *src,size_t bytes){
 size_t i=0;while(dst[i])i++;for(size_t j=0;j<bytes&&src[j];j++)dst[i++]=src[j];dst[i]=0;return dst;
}
char *sf_strchr(const char *s,int c){
 for(;;s++){if(*s==(char)c)return(char*)s;if(!*s)return 0;}
}
char *sf_strrchr(const char *s,int c){
 const char*last=0;for(;;s++){if(*s==(char)c)last=s;if(!*s)break;}return(char*)last;
}
char *sf_strstr(const char *text,const char *word){
 if(!*word)return(char*)text;for(;*text;text++){
  size_t i=0;while(word[i]&&text[i]==word[i])i++;if(!word[i])return(char*)text;
 }
 return 0;
}
size_t sf_strspn(const char *s,const char *accept){
 size_t n=0;while(s[n]){const char*a=accept;while(*a&&*a!=s[n])a++;if(!*a)break;n++;}return n;
}
size_t sf_strcspn(const char *s,const char *reject){
 size_t n=0;while(s[n]){const char*r=reject;while(*r&&*r!=s[n])r++;if(*r)break;n++;}return n;
}
char *sf_strpbrk(const char *s,const char *accept){
 for(;*s;s++){const char*a=accept;while(*a){if(*a==*s)return(char*)s;a++;}}return 0;
}
char *sf_strtok_r(char *string,const char *delim,char **cursor){
 char*p=string?string:*cursor;if(!p)return 0;p+=sf_strspn(p,delim);
 if(!*p){*cursor=p;return 0;}
 char*token=p;p+=sf_strcspn(p,delim);if(*p)*p++=0;*cursor=p;return token;
}
#define SF_U 0x01u
#define SF_L 0x02u
#define SF_D 0x04u
#define SF_S 0x08u
#define SF_P 0x10u
#define SF_C 0x20u
#define SF_X 0x40u
#define SF_G 0x80u
/* Computed instead of a 128-entry table so a single definition owns ASCII. */
static unsigned sf_class(int c){
 if((unsigned)c>127u)return 0;
 if(c<32)return(c=='\t'||c=='\n'||c=='\v'||c=='\f'||c=='\r')?SF_C|SF_S:SF_C;
 if(c==32)return SF_S;
 if(c==127)return SF_C;
 if(c>='0'&&c<='9')return SF_D|SF_X|SF_G;
 if(c>='A'&&c<='Z')return SF_U|SF_G|(c<='F'?SF_X:0u);
 if(c>='a'&&c<='z')return SF_L|SF_G|(c<='f'?SF_X:0u);
 return SF_P|SF_G;
}
int sf_isalpha(int c){return(int)(sf_class(c)&(SF_U|SF_L));}
int sf_isdigit(int c){return(int)(sf_class(c)&SF_D);}
int sf_isalnum(int c){return(int)(sf_class(c)&(SF_U|SF_L|SF_D));}
int sf_isspace(int c){return(int)(sf_class(c)&SF_S);}
int sf_isupper(int c){return(int)(sf_class(c)&SF_U);}
int sf_islower(int c){return(int)(sf_class(c)&SF_L);}
int sf_isprint(int c){return c>=32&&c<127;}
int sf_ispunct(int c){return(int)(sf_class(c)&SF_P);}
int sf_isxdigit(int c){return(int)(sf_class(c)&SF_X);}
int sf_isgraph(int c){return(int)(sf_class(c)&SF_G);}
int sf_iscntrl(int c){return(int)(sf_class(c)&SF_C);}
int sf_toupper(int c){return sf_islower(c)?c-32:c;}
int sf_tolower(int c){return sf_isupper(c)?c+32:c;}
int sf_abs(int value){return value<0?(int)(0u-(unsigned)value):value;}
long sf_labs(long value){return value<0?(long)(0ul-(unsigned long)value):value;}
static int sf_digit(int c,int base){
 int value;
 if(c>='0'&&c<='9')value=c-'0';else if(c>='a'&&c<='z')value=c-'a'+10;else if(c>='A'&&c<='Z')value=c-'A'+10;else return-1;
 return value<base?value:-1;
}
long sf_strtol(const char *text,char **end,int base){
 const char*p=text;while(sf_isspace((unsigned char)*p))p++;
 int negative=0;if(*p=='+'||*p=='-')negative=*p++=='-';
 /* A 0x prefix is consumed only when a hex digit follows, as C requires. */
 if((base==0||base==16)&&p[0]=='0'&&(p[1]=='x'||p[1]=='X')&&sf_digit((unsigned char)p[2],16)>=0){base=16;p+=2;}
 else if(base==0)base=p[0]=='0'?8:10;
 if(base<2||base>36){if(end)*end=(char*)text;return 0;}
 unsigned long limit=negative?(unsigned long)SF_LONG_MAX+1ul:(unsigned long)SF_LONG_MAX;
 unsigned long value=0;int any=0,overflow=0;
 for(;;){int digit=sf_digit((unsigned char)*p,base);if(digit<0)break;any=1;
  if(value>(limit-(unsigned long)digit)/(unsigned long)base)overflow=1;
  else value=value*(unsigned long)base+(unsigned long)digit;
  p++;
 }
 if(end)*end=(char*)(any?p:text);
 if(overflow)return negative?SF_LONG_MIN:SF_LONG_MAX;
 if(negative)return value==(unsigned long)SF_LONG_MAX+1ul?SF_LONG_MIN:-(long)value;
 return(long)value;
}
unsigned long sf_strtoul(const char *text,char **end,int base){
 const char*p=text;while(sf_isspace((unsigned char)*p))p++;
 int negative=0;if(*p=='+'||*p=='-')negative=*p++=='-';
 /* A 0x prefix is consumed only when a hex digit follows, as C requires. */
 if((base==0||base==16)&&p[0]=='0'&&(p[1]=='x'||p[1]=='X')&&sf_digit((unsigned char)p[2],16)>=0){base=16;p+=2;}
 else if(base==0)base=p[0]=='0'?8:10;
 if(base<2||base>36){if(end)*end=(char*)text;return 0;}
 unsigned long limit=~0ul,value=0;int any=0,overflow=0;
 for(;;){int digit=sf_digit((unsigned char)*p,base);if(digit<0)break;any=1;
  if(value>(limit-(unsigned long)digit)/(unsigned long)base)overflow=1;
  else value=value*(unsigned long)base+(unsigned long)digit;
  p++;
 }
 if(end)*end=(char*)(any?p:text);
 if(overflow)return limit;
 return negative?(unsigned long)(0ul-value):value;
}
int sf_atoi(const char *text){return(int)sf_strtol(text,0,10);}
long sf_atol(const char *text){return sf_strtol(text,0,10);}
void *sf_bsearch(const void *key,const void *base,size_t count,size_t size,int(*compare)(const void*,const void*)){
 const char*data=base;size_t low=0,high=count;
 while(low<high){size_t middle=low+(high-low)/2;const void*item=data+middle*size;int order=compare(key,item);
  if(!order)return(void*)item;if(order<0)high=middle;else low=middle+1;
 }
 return 0;
}
typedef struct {char *buffer;size_t size,written;} SfOut;
static void sf_emit(SfOut*out,char c){
 if(out->buffer&&out->size>1&&out->written<out->size-1)out->buffer[out->written]=c;out->written++;
}
static void sf_repeat(SfOut*out,char c,unsigned count){while(count--)sf_emit(out,c);}
#define SF_LEFT 1
#define SF_ZERO 2
#define SF_PLUS 4
#define SF_BLANK 8
static unsigned sf_decimal(uint64_t value,char digits[20]){
 char reverse[20];unsigned n=0;do{reverse[n++]=(char)('0'+value%10);value/=10;}while(value);
 for(unsigned i=0;i<n;i++)digits[i]=reverse[n-1-i];return n;
}
static unsigned sf_hexadecimal(uint64_t value,char digits[16],int upper){
 static const char lower[17]="0123456789abcdef",upper_digits[17]="0123456789ABCDEF";
 const char*table=upper?upper_digits:lower;char reverse[16];unsigned n=0;
 do{reverse[n++]=table[value&15u];value>>=4;}while(value);
 for(unsigned i=0;i<n;i++)digits[i]=reverse[n-1-i];return n;
}
static unsigned sf_octal(uint64_t value,char digits[22]){
 char reverse[22];unsigned n=0;do{reverse[n++]=(char)('0'+(value&7u));value>>=3;}while(value);
 for(unsigned i=0;i<n;i++)digits[i]=reverse[n-1-i];return n;
}
static void sf_number(SfOut*out,uint64_t value,int base,int upper,int negative,int flags,unsigned width,int precision,const char*prefix){
 char digits[22];unsigned n=base==8?sf_octal(value,digits):base==16?sf_hexadecimal(value,digits,upper):sf_decimal(value,digits);
 if(precision==0&&!value)n=0;
 unsigned zeros=precision>=0&&(unsigned)precision>n?(unsigned)precision-n:0;
 const char*sign=negative?"-":(flags&SF_PLUS)?"+":(flags&SF_BLANK)?" ":"";unsigned sign_len=sign[0]?1u:0u;
 unsigned prefix_len=prefix?2u:0u,total=sign_len+prefix_len+zeros+n;
 if((unsigned)width>total){
  unsigned pad=(unsigned)width-total;
  if(flags&SF_LEFT){if(sign_len)sf_emit(out,sign[0]);if(prefix_len){sf_emit(out,prefix[0]);sf_emit(out,prefix[1]);}sf_repeat(out,'0',zeros);for(unsigned i=0;i<n;i++)sf_emit(out,digits[i]);sf_repeat(out,' ',pad);return;}
  if((flags&SF_ZERO)&&precision<0){pad=0;zeros=(unsigned)width-(sign_len+prefix_len+n);}
  else sf_repeat(out,' ',pad);
 }
 if(sign_len)sf_emit(out,sign[0]);
 if(prefix_len){sf_emit(out,prefix[0]);sf_emit(out,prefix[1]);}
 sf_repeat(out,'0',zeros);for(unsigned i=0;i<n;i++)sf_emit(out,digits[i]);
}
int sf_vsnprintf(char *buffer,size_t size,const char *format,va_list args){
 SfOut out={buffer,size,0};
 while(*format){
  if(*format!='%'){sf_emit(&out,*format++);continue;}
  format++;
  int flags=0;
  for(;;format++){if(*format=='-')flags|=SF_LEFT;else if(*format=='0')flags|=SF_ZERO;else if(*format=='+')flags|=SF_PLUS;else if(*format==' ')flags|=SF_BLANK;else break;}
  unsigned width=0;unsigned width_digits=0;
  while(*format>='0'&&*format<='9'){if(width_digits++<3)width=width*10u+(unsigned)(*format-'0');format++;}
  if(width>SF_STRLEN_MAX)width=SF_STRLEN_MAX;
  int precision=-1;
  if(*format=='.'){format++;precision=0;unsigned digits=0;while(*format>='0'&&*format<='9'){if(digits++<3)precision=precision*10+(int)(*format-'0');format++;}if(precision>(int)SF_STRLEN_MAX)precision=(int)SF_STRLEN_MAX;}
  int length=0;
  if(*format=='h'){length=1;format++;if(*format=='h'){length=2;format++;}}
  else if(*format=='l'){length=3;format++;if(*format=='l'){length=4;format++;}}
  else if(*format=='z'||*format=='t'||*format=='j'){length=5;format++;}
  char conversion=*format;
  if(!conversion){sf_emit(&out,'%');break;}
  format++;
  if(conversion=='d'||conversion=='i'){
   long long value;
   if(length==4)value=va_arg(args,long long);else if(length==3)value=va_arg(args,long);
   else if(length==2)value=(signed char)va_arg(args,int);else if(length==1)value=(short)va_arg(args,int);else value=va_arg(args,int);
   uint64_t magnitude=value<0?(uint64_t)(-(value+1))+1u:(uint64_t)value;
   sf_number(&out,magnitude,10,0,value<0,flags,width,precision,0);
  }else if(conversion=='u'||conversion=='o'||conversion=='x'||conversion=='X'){
   unsigned long long value;
   if(length==4)value=va_arg(args,unsigned long long);else if(length==3)value=va_arg(args,unsigned long);
   else if(length==5)value=va_arg(args,size_t);else if(length==2)value=(unsigned char)va_arg(args,unsigned int);
   else if(length==1)value=(unsigned short)va_arg(args,unsigned int);else value=va_arg(args,unsigned int);
   int base=conversion=='o'?8:conversion=='u'?10:16;
   /* '+' and ' ' are only meaningful for signed conversions; libc ignores them here. */
   sf_number(&out,(uint64_t)value,base,conversion=='X',0,flags&~(SF_PLUS|SF_BLANK),width,precision,0);
  }else if(conversion=='p'){
   void*pointer=va_arg(args,void*);sf_number(&out,(uint64_t)(uintptr_t)pointer,16,0,0,flags,width,-1,"0x");
  }else if(conversion=='c'){
   char c=(char)va_arg(args,int);if((unsigned)width>1&&!(flags&SF_LEFT))sf_repeat(&out,' ',(unsigned)width-1);sf_emit(&out,c);if((unsigned)width>1&&(flags&SF_LEFT))sf_repeat(&out,' ',(unsigned)width-1);
  }else if(conversion=='s'){
   const char*text=va_arg(args,const char*);if(!text)text="(null)";
   unsigned n=0;while(text[n]&&(precision<0||(int)n<precision)&&n<SF_STRLEN_MAX)n++;
   unsigned pad=(unsigned)width>n?(unsigned)width-n:0;
   if(pad&&!(flags&SF_LEFT))sf_repeat(&out,' ',pad);for(unsigned i=0;i<n;i++)sf_emit(&out,text[i]);if(pad&&(flags&SF_LEFT))sf_repeat(&out,' ',pad);
  }else if(conversion=='%'){
   sf_emit(&out,'%');
  }else{
   sf_emit(&out,'%');sf_emit(&out,conversion);
  }
 }
 if(out.size)out.buffer[out.written<out.size?out.written:out.size-1]=0;
 return(int)out.written;
}
int sf_snprintf(char *buffer,size_t size,const char *format,...){
 va_list args;va_start(args,format);int result=sf_vsnprintf(buffer,size,format,args);va_end(args);return result;
}
