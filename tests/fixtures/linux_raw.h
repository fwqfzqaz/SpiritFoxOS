#ifndef SF_LINUX_RAW_H
#define SF_LINUX_RAW_H
/* 静态 PIE 夹具的原始 Linux x86_64 调用封装。
 * 装载器会把 .text 中的 0F 05 改写成 CD 80，这里直接写 syscall 指令即可。 */
#include "linux_abi.h"

static inline long lx_raw(long number,long a,long b,long c,long d,long e,long f){
 register long r10 __asm__("r10")=d;
 register long r8 __asm__("r8")=e;
 register long r9 __asm__("r9")=f;
 long result;
 __asm__ volatile("syscall":"=a"(result)
                  :"a"(number),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9)
                  :"rcx","r11","memory");
 return result;
}

static inline unsigned long lx_strlen(const char *s){
 unsigned long n=0;
 while(s[n])n++;
 return n;
}

static inline long lx_write(int fd,const void *buffer,unsigned long bytes){
 return lx_raw(LX_SYS_WRITE,fd,(long)buffer,(long)bytes,0,0,0);
}

static inline void lx_puts(const char *s){lx_write(1,s,lx_strlen(s));}

static inline void lx_putnum(long value){
 char digits[20];
 int count=0;
 if(value<0){lx_puts("-");value=-value;}
 if(!value)digits[count++]='0';
 while(value){digits[count++]=(char)('0'+value%10);value/=10;}
 char out[20];
 for(int i=0;i<count;i++)out[i]=digits[count-1-i];
 lx_write(1,out,(unsigned long)count);
}

static inline int lx_same(const char *a,const char *b){
 while(*a&&*a==*b){a++;b++;}
 return *a==*b;
}

#endif
