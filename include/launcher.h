#ifndef LAUNCHER_H
#define LAUNCHER_H
#include <stdint.h>
/* 有界应用目录与搜索启动器：固定容量、无堆、无 libc。
 * 目录只记录应用 id/标题与已打开的窗口 id；搜索为大小写不敏感子串匹配。 */
#define LAUNCHER_APPS 16u
#define LAUNCHER_ID_BYTES 40u
#define LAUNCHER_TITLE_BYTES 24u
#define LAUNCHER_QUERY_BYTES 24u
enum { LAUNCHER_OK=0, LAUNCHER_INVALID=-1, LAUNCHER_NO_SPACE=-2, LAUNCHER_NOT_FOUND=-3 };

typedef struct {
 char id[LAUNCHER_ID_BYTES];
 char title[LAUNCHER_TITLE_BYTES];
 uint64_t window;                 /* 0 表示尚未打开 */
} LauncherEntry;

typedef struct {
 uint32_t count, selected, query_len;
 char query[LAUNCHER_QUERY_BYTES];
 LauncherEntry entries[LAUNCHER_APPS];
} Launcher;

void launcher_init(Launcher*);
/* 重复 id 只更新标题并保留已记录的窗口；非法输入或目录满时状态不变。 */
int launcher_add(Launcher*,const char*id,const char*title);
uint32_t launcher_count(const Launcher*);
const LauncherEntry* launcher_at(const Launcher*,uint32_t index);
/* 返回匹配总数；out 容量不足时只写前 capacity 个稳定索引。 */
uint32_t launcher_search(const Launcher*,uint32_t*out,uint32_t capacity);
int launcher_query_push(Launcher*,char c);
int launcher_query_pop(Launcher*);
void launcher_query_clear(Launcher*);
int launcher_select(Launcher*,int delta);
int launcher_set_window(Launcher*,const char*id,uint64_t window);
#endif
