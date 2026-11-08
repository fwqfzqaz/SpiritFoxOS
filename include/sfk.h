#ifndef SFK_H
#define SFK_H
#include <stdint.h>
/* 有界图形 shell（SFK）核心：固定输入缓冲与滚动历史，无堆、无 libc。
 * 合成器只负责绘制与执行动作；解析与历史上限全部在这里做宿主可测。 */
#define SFK_INPUT_BYTES 48u
#define SFK_HISTORY_LINES 12u
#define SFK_HISTORY_BYTES 64u
enum { SFK_OK=0, SFK_INVALID=-1, SFK_FULL=-2, SFK_EMPTY=-3 };

enum { SFK_NONE=0, SFK_HELP, SFK_APPS, SFK_OPEN, SFK_CLOSE, SFK_WS,
       SFK_LAYOUT, SFK_SEARCH, SFK_CLEAR, SFK_ECHO, SFK_ABOUT, SFK_EXIT,
       SFK_NEW, SFK_UNKNOWN };

typedef struct {
 uint32_t action;      /* SFK_* 命令动作 */
 uint32_t arg;         /* WS 1..4 / LAYOUT 千分比；无法解析时为 0 */
 const char*line;      /* 原始输入，submit 后保存在 Sfk 内 */
 const char*text;      /* 参数文本；UNKNOWN 时为命令词 */
} SfkCommand;

typedef struct {
 uint32_t input_len;
 char input[SFK_INPUT_BYTES];
 uint32_t history_start, history_len;
 char history[SFK_HISTORY_LINES][SFK_HISTORY_BYTES];
 char last_line[SFK_INPUT_BYTES];
 char last_arg[SFK_INPUT_BYTES];
} Sfk;

void sfk_init(Sfk*);
/* 只接受可打印 ASCII；缓冲满返回 SFK_FULL 且状态不变。 */
int sfk_push(Sfk*,char);
int sfk_backspace(Sfk*);
void sfk_clear_input(Sfk*);
/* 追加一行历史；超过 63 字节或空指针返回 SFK_INVALID 且状态不变。 */
int sfk_print(Sfk*,const char*);
void sfk_clear(Sfk*);
/* 保存输入、解析命令并清空输入；空输入返回 SFK_NONE。
 * 参数缺失或非法不产生副作用，调用方据 action/arg 给出用法提示。 */
int sfk_submit(Sfk*,SfkCommand*);
uint32_t sfk_line_count(const Sfk*);
const char* sfk_line_at(const Sfk*,uint32_t index);
#endif
