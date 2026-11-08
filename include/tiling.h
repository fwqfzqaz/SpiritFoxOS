#ifndef TILING_H
#define TILING_H
#include <stdint.h>
/* 类 niri 的可滚动平铺布局核心：固定容量、无堆、无 libc。
 * 工作区包含若干列，列内窗口纵向堆叠；列从左右排列，视口可水平滚动。 */
#define TILE_WORKSPACES 4u
#define TILE_COLUMNS 16u
#define TILE_WINDOWS 8u
#define TILE_WIDTH_MIN 250u
#define TILE_WIDTH_MAX 3000u
#define TILE_HEIGHT_MIN 250u
#define TILE_HEIGHT_MAX 3000u
enum { TILE_OK=0, TILE_INVALID=-1, TILE_DENIED=-2, TILE_NO_SPACE=-3, TILE_EMPTY=-4, TILE_NOT_FOUND=-5 };

typedef struct { uint64_t id; } TileWindow;
typedef struct {
 uint32_t count, active;      /* 窗口数 / 焦点窗口索引 */
 uint32_t width_permille;     /* 列宽千分比，默认 500 */
 uint32_t height_permille;    /* 窗口高度千分比，默认 1000 */
 TileWindow windows[TILE_WINDOWS];
} TileColumn;
typedef struct {
 uint32_t count, active;      /* 列数 / 焦点列索引 */
 int32_t scroll;              /* 内容滚动像素，始终 >= 0 */
 TileColumn columns[TILE_COLUMNS];
} TileWorkspace;
typedef struct {
 uint32_t viewport_width, viewport_height;
 uint32_t outer_gap, gap, bar_height;
 uint32_t active;             /* 活动工作区索引 */
 uint64_t next_id;
 TileWorkspace workspaces[TILE_WORKSPACES];
} TileLayout;
/* 未裁剪的窗口几何；visible 表示是否与内容区相交。 */
typedef struct { int32_t x, y; uint32_t width, height, visible; } TileRect;

void tile_init(TileLayout*,uint32_t viewport_width,uint32_t viewport_height,
               uint32_t bar_height,uint32_t outer_gap,uint32_t gap);
int tile_add(TileLayout*,uint64_t*out_id);
int tile_close_focused(TileLayout*);
int tile_consume(TileLayout*);
int tile_expel(TileLayout*);
int tile_move_column(TileLayout*,int delta);
int tile_focus_window(TileLayout*,int delta);
/* 列内交换焦点窗口与相邻窗口（niri move-window-up/down）；越界返回 TILE_DENIED 且状态不变。 */
int tile_swap_window(TileLayout*,int delta);
int tile_focus_column(TileLayout*,int delta);
int tile_focus_workspace(TileLayout*,int delta);
int tile_set_width(TileLayout*,int delta_permille);
int tile_set_height(TileLayout*,int delta_permille);
int tile_scroll_by(TileLayout*,int delta_pixels);
int tile_scroll_to_focus(TileLayout*);
/* 输出活动工作区的窗口矩形；容量不足时不写 out，*count 置为所需数量并返回
 * TILE_NO_SPACE。空工作区返回 TILE_EMPTY 且 *count=0。 */
int tile_layout(TileLayout*,TileRect*out,uint32_t capacity,uint32_t*count);
uint64_t tile_focused_id(const TileLayout*);
uint32_t tile_scroll(const TileLayout*);
/* 跨工作区/列/窗口聚焦指定 id；未命中返回 TILE_NOT_FOUND 且不修改状态。 */
int tile_focus_id(TileLayout*,uint64_t id);
#endif
