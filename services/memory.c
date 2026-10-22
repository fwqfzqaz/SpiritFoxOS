#include "sf.h"
void *memcpy(void *dst,const void *src,size_t n) { uint8_t *d=dst;const uint8_t*s=src;while(n--)*d++=*s++;return dst; }
void *memset(void *dst,int c,size_t n) { uint8_t*d=dst;while(n--)*d++=(uint8_t)c;return dst; }
int memcmp(const void*a,const void*b,size_t n) { const uint8_t*x=a,*y=b;while(n--){if(*x!=*y)return *x-*y;x++;y++;}return 0; }
size_t strlen(const char*s) { size_t n=0;while(s[n])n++;return n; }
int strcmp(const char*a,const char*b) { while(*a&&*a==*b){a++;b++;}return (uint8_t)*a-(uint8_t)*b; }
