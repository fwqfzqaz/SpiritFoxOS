#include "launcher.h"
/* 目录与搜索实现：所有状态位于 Launcher 内，固定数组、无动态分配。
 * 比较只针对可打印 ASCII，大小写折叠由 ascii_lower 完成。 */
static int ascii_lower(int c){return c>='A'&&c<='Z'?c+32:c;}
static uint32_t text_len(const char*s){uint32_t n=0;if(!s)return 0;while(s[n])n++;return n;}
static int equal_id(const char*a,const char*b){
 if(!a||!b)return 0;
 for(uint32_t i=0;;i++){if(a[i]!=b[i])return 0;if(!a[i])return 1;}
}
/* 字段长度必须留下结尾 NUL，超长视为非法。 */
static int copy_field(char*dst,uint32_t capacity,const char*src){
 if(!src||text_len(src)>=capacity)return LAUNCHER_INVALID;
 uint32_t i=0;for(;src[i];i++)dst[i]=src[i];
 dst[i]=0;return LAUNCHER_OK;
}
static int contains(const char*hay,const char*needle,uint32_t nlen){
 for(uint32_t i=0;hay[i];i++){
  uint32_t j=0;
  while(j<nlen&&hay[i+j]&&ascii_lower(hay[i+j])==ascii_lower(needle[j]))j++;
  if(j==nlen)return 1;
 }
 return 0;
}
static int entry_matches(const LauncherEntry*e,const Launcher*l){
 if(!l->query_len)return 1;
 return contains(e->id,l->query,l->query_len)||contains(e->title,l->query,l->query_len);
}
void launcher_init(Launcher*l){
 if(!l)return;
 uint8_t*bytes=(uint8_t*)l;
 for(uint32_t i=0;i<sizeof(*l);i++)bytes[i]=0;
}
int launcher_add(Launcher*l,const char*id,const char*title){
 if(!l||!id||!id[0]||!title)return LAUNCHER_INVALID;
 if(text_len(id)>=LAUNCHER_ID_BYTES||text_len(title)>=LAUNCHER_TITLE_BYTES)return LAUNCHER_INVALID;
 for(uint32_t i=0;i<l->count;i++){
  if(equal_id(l->entries[i].id,id))return copy_field(l->entries[i].title,LAUNCHER_TITLE_BYTES,title);
 }
 if(l->count>=LAUNCHER_APPS)return LAUNCHER_NO_SPACE;
 LauncherEntry*e=&l->entries[l->count];
 int r=copy_field(e->id,LAUNCHER_ID_BYTES,id);
 if(r)return r;
 r=copy_field(e->title,LAUNCHER_TITLE_BYTES,title);
 if(r)return r;
 e->window=0;
 l->count++;
 return LAUNCHER_OK;
}
uint32_t launcher_count(const Launcher*l){return l?l->count:0;}
const LauncherEntry*launcher_at(const Launcher*l,uint32_t index){
 if(!l||index>=l->count)return 0;
 return &l->entries[index];
}
uint32_t launcher_search(const Launcher*l,uint32_t*out,uint32_t capacity){
 if(!l)return 0;
 uint32_t matches=0;
 for(uint32_t i=0;i<l->count;i++){
  if(!entry_matches(&l->entries[i],l))continue;
  if(out&&matches<capacity)out[matches]=i;
  matches++;
 }
 return matches;
}
int launcher_query_push(Launcher*l,char c){
 if(!l||c<0x20||c>0x7e)return LAUNCHER_INVALID;
 if(l->query_len+1u>=LAUNCHER_QUERY_BYTES)return LAUNCHER_NO_SPACE;
 l->query[l->query_len++]=(char)c;
 l->query[l->query_len]=0;
 l->selected=0;
 return LAUNCHER_OK;
}
int launcher_query_pop(Launcher*l){
 if(!l)return LAUNCHER_INVALID;
 if(!l->query_len)return LAUNCHER_NOT_FOUND;
 l->query_len--;
 l->query[l->query_len]=0;
 l->selected=0;
 return LAUNCHER_OK;
}
void launcher_query_clear(Launcher*l){
 if(!l)return;
 l->query_len=0;l->query[0]=0;l->selected=0;
}
int launcher_select(Launcher*l,int delta){
 if(!l)return LAUNCHER_INVALID;
 uint32_t matches_count=launcher_search(l,0,0);
 if(!matches_count){l->selected=0;return LAUNCHER_OK;}
 int64_t target=(int64_t)l->selected+delta;
 if(target<0)target=0;
 if(target>=(int64_t)matches_count)target=(int64_t)matches_count-1;
 l->selected=(uint32_t)target;
 return LAUNCHER_OK;
}
int launcher_set_window(Launcher*l,const char*id,uint64_t window){
 if(!l||!id||!id[0]||!window)return LAUNCHER_INVALID;
 for(uint32_t i=0;i<l->count;i++)
  if(equal_id(l->entries[i].id,id)){l->entries[i].window=window;return LAUNCHER_OK;}
 return LAUNCHER_NOT_FOUND;
}
