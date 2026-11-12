#include "libsf.h"
/* 基于 SYS_MMAP 的进程内堆：每个区域（Region）从匿名映射取得，内部按
 * 32 字节块头切成块并维护按地址排序的空闲链表，释放时合并相邻块。
 * 供导入 sf-libsf 的应用使用；不保证线程安全（由调用者串行化）。 */
#define ALIGN16(n) (((n)+15u)&~15u)
#define HEADER 32u
typedef struct Block { uint64_t size; struct Block*next; uint32_t used,pad; uint64_t reserved; } Block;
typedef struct Region { struct Region*next; uint64_t bytes; Block*free; uint64_t reserved; } Region;
static Region *regions;
static uint64_t page_up(uint64_t n){return(n+4095u)&~4095ull;}
/* 按地址顺序插入空闲块，供分配时首次适配与释放时向前合并。 */
static void free_insert(Region*r,Block*b){
 Block**p=&r->free;while(*p&&*p<b)p=&(*p)->next;b->next=*p;*p=b;
}
static Region *region_of(Block*b){
 for(Region*r=regions;r;r=r->next)if((uint8_t*)b>=(uint8_t*)r&&(uint8_t*)b<(uint8_t*)r+r->bytes)return r;
 return 0;
}
/* 新区域：整段未映射的匿名页，首个块覆盖除 Region 头以外的全部空间。 */
static Region *region_new(uint64_t need){
 uint64_t bytes=page_up(need+sizeof(Region));
 if(bytes<65536)bytes=65536;
 long base=sf_mmap(0,bytes);
 if(base<0)return 0;
 Region*r=(Region*)base;r->next=regions;r->bytes=bytes;r->free=0;regions=r;
 Block*b=(Block*)((uint8_t*)base+sizeof(Region));
 b->size=bytes-sizeof(Region);b->next=0;b->used=0;b->pad=0;b->reserved=0;
 free_insert(r,b);
 return r;
}
static void *heap_alloc(uint64_t n){
 if(!n)n=1;
 uint64_t need=HEADER+ALIGN16(n);
 for(;;){
  for(Region*r=regions;r;r=r->next){
   Block*prev=0;
   for(Block*b=r->free;b;prev=b,b=b->next){
    if(b->size<need)continue;
    if(prev)prev->next=b->next;else r->free=b->next;
    if(b->size>=need+HEADER+16){
     Block*rest=(Block*)((uint8_t*)b+need);
     rest->size=b->size-need;rest->next=0;rest->used=0;rest->pad=0;rest->reserved=0;
     b->size=need;free_insert(r,rest);
    }
    b->used=1;b->next=0;
    return (uint8_t*)b+HEADER;
   }
  }
  if(!region_new(need))return 0;
 }
}
void free(void*p){
 if(!p)return;
 Block*b=(Block*)((uint8_t*)p-HEADER);
 if(!b->used)return;
 Region*r=region_of(b);if(!r)return;
 b->used=0;
 /* 与后继合并：后继必须正好在 b 之后且空闲。 */
 Block*next=(Block*)((uint8_t*)b+b->size);
 if((uint8_t*)next<(uint8_t*)r+r->bytes&&!next->used){
  Block**q=&r->free;while(*q&&*q!=next)q=&(*q)->next;
  if(*q){*q=next->next;b->size+=next->size;}
 }
 /* 与前驱合并：空闲链表中 b 的前一项正好结束于 b。 */
 Block*prev=0;for(Block*q=r->free;q&&q<b;q=q->next)prev=q;
 if(prev&&(uint8_t*)prev+prev->size==(uint8_t*)b){prev->size+=b->size;b=prev;}
 else free_insert(r,b);
}
void *malloc(uint64_t n){return heap_alloc(n);}
void *calloc(uint64_t count,uint64_t size){
 if(size&&count>(uint64_t)-1/size)return 0;
 uint64_t n=count*size;void*p=heap_alloc(n);
 if(p&&n)memset(p,0,n);
 return p;
}
void *realloc(void*p,uint64_t n){
 if(!p)return heap_alloc(n);
 if(!n){free(p);return 0;}
 Block*b=(Block*)((uint8_t*)p-HEADER);
 if(b->size-HEADER>=n)return p;
 void*q=heap_alloc(n);
 if(!q)return 0;
 memcpy(q,p,b->size-HEADER);
 free(p);
 return q;
}
