#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tiling.h"
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"TILING FAIL line %d\n",__LINE__);exit(1);}}while(0)
static TileLayout *fresh(void){
 static TileLayout layout;
 tile_init(&layout,1280,800,24,12,8);
 return &layout;
}
int main(void){
 TileLayout *l=fresh();uint64_t id=0;
 /* 初始状态 */
 CHECK(l->active==0&&l->next_id==1);
 for(unsigned i=0;i<TILE_WORKSPACES;i++){
  CHECK(!l->workspaces[i].count&&!l->workspaces[i].active&&!l->workspaces[i].scroll);
  for(unsigned j=0;j<TILE_COLUMNS;j++){
   CHECK(!l->workspaces[i].columns[j].count);
   CHECK(l->workspaces[i].columns[j].width_permille==500);
   CHECK(l->workspaces[i].columns[j].height_permille==1000);
  }
 }
 TileRect rects[64];uint32_t count=99;
 CHECK(tile_layout(l,rects,64,&count)==TILE_EMPTY&&!count);
 CHECK(tile_focused_id(l)==0&&tile_scroll(l)==0);

 /* 新增窗口：首窗口建列，后续插入焦点窗口之后 */
 CHECK(tile_add(l,&id)==TILE_OK&&id==1);
 CHECK(l->workspaces[0].count==1&&l->workspaces[0].active==0);
 CHECK(l->workspaces[0].columns[0].count==1&&l->workspaces[0].columns[0].active==0);
 CHECK(tile_add(l,&id)==TILE_OK&&id==2);
 CHECK(l->workspaces[0].columns[0].count==2&&l->workspaces[0].columns[0].active==1);
 CHECK(tile_focused_id(l)==2);
 CHECK(tile_add(l,&id)==TILE_OK&&id==3);
 CHECK(l->workspaces[0].columns[0].count==3&&l->workspaces[0].columns[0].active==2);

 /* 列内焦点夹紧 */
 CHECK(tile_focus_window(l,-1)==TILE_OK&&l->workspaces[0].columns[0].active==1);
 CHECK(tile_focus_window(l,-9)==TILE_OK&&l->workspaces[0].columns[0].active==0);
 CHECK(tile_focus_window(l,9)==TILE_OK&&l->workspaces[0].columns[0].active==2);
 CHECK(tile_focus_window(l,1)==TILE_OK&&l->workspaces[0].columns[0].active==2);

 /* 布局：单列三窗口，等高、间距、纵向堆叠、可见 */
 CHECK(tile_layout(l,rects,64,&count)==TILE_OK&&count==3);
 CHECK(rects[0].x==12&&rects[0].y==24&&rects[0].width==640);
 {
  uint32_t base=(800-24-(3-1)*8)/3;
  CHECK(rects[0].height==base);
  for(uint32_t i=1;i<3;i++){
   CHECK(rects[i].x==rects[0].x&&rects[i].width==rects[0].width);
   CHECK(rects[i].y==rects[i-1].y+(int32_t)rects[i-1].height+8);
   CHECK(rects[i].visible==1);
  }
 }
 /* 容量不足：不写缓冲、返回所需数量 */
 {
  TileRect guard[1]={{7,7,7,7,7}};uint32_t need=0;
  CHECK(tile_layout(l,guard,1,&need)==TILE_NO_SPACE&&need==3);
  CHECK(guard[0].x==7&&guard[0].y==7&&guard[0].width==7&&guard[0].height==7);
 }

 /* 关闭窗口与空列回收 */
 CHECK(tile_close_focused(l)==TILE_OK);
 CHECK(l->workspaces[0].columns[0].count==2&&l->workspaces[0].columns[0].active==1);
 CHECK(tile_close_focused(l)==TILE_OK);
 CHECK(tile_close_focused(l)==TILE_OK);
 CHECK(!l->workspaces[0].count&&!l->workspaces[0].columns[0].count);
 CHECK(l->workspaces[0].active==0&&l->workspaces[0].scroll==0);
 CHECK(tile_close_focused(l)==TILE_EMPTY);

 /* expel：两窗口一列 → 拆成两列，焦点移到新列 */
 l=fresh();
 CHECK(tile_add(l,&id)==TILE_OK&&id==1);
 CHECK(tile_add(l,&id)==TILE_OK&&id==2);
 CHECK(tile_expel(l)==TILE_OK);
 CHECK(l->workspaces[0].count==2&&l->workspaces[0].active==1);
 CHECK(l->workspaces[0].columns[0].count==1&&l->workspaces[0].columns[0].windows[0].id==1);
 CHECK(l->workspaces[0].columns[1].count==1&&l->workspaces[0].columns[1].windows[0].id==2);
 CHECK(tile_focused_id(l)==2);
 /* 单窗口列不能 expel */
 CHECK(tile_expel(l)==TILE_DENIED&&l->workspaces[0].count==2);

 /* consume：焦点窗口并入左侧列；最左列拒绝；空列回收 */
 CHECK(tile_add(l,&id)==TILE_OK&&id==3); /* 列 1 变为 2 窗口，焦点 id3 */
 CHECK(tile_consume(l)==TILE_OK);
 CHECK(l->workspaces[0].count==2&&l->workspaces[0].active==0);
 CHECK(l->workspaces[0].columns[0].count==2);
 CHECK(l->workspaces[0].columns[0].windows[0].id==1);
 CHECK(l->workspaces[0].columns[0].windows[1].id==3);
 CHECK(l->workspaces[0].columns[1].count==1&&l->workspaces[0].columns[1].windows[0].id==2);
 CHECK(tile_focused_id(l)==3);
 CHECK(tile_consume(l)==TILE_DENIED);
 /* 单窗口源列被 consume 后整列回收 */
 l=fresh();
 CHECK(tile_add(l,&id)==TILE_OK);
 CHECK(tile_add(l,&id)==TILE_OK);
 CHECK(tile_expel(l)==TILE_OK);  /* 列0:id1, 列1:id2(焦点) */
 CHECK(tile_add(l,&id)==TILE_OK); /* 列1:id2,id3 */
 CHECK(tile_expel(l)==TILE_OK);  /* 列1:id2, 列2:id3(焦点) */
 CHECK(l->workspaces[0].count==3&&l->workspaces[0].active==2);
 CHECK(tile_consume(l)==TILE_OK);
 CHECK(l->workspaces[0].count==2&&l->workspaces[0].active==1);
 CHECK(l->workspaces[0].columns[1].count==2);
 CHECK(l->workspaces[0].columns[1].windows[1].id==3);

 /* 列移动与焦点列夹紧 */
 l=fresh();
 CHECK(tile_add(l,&id)==TILE_OK);CHECK(tile_add(l,&id)==TILE_OK);
 CHECK(tile_expel(l)==TILE_OK);          /* 列 0:id1, 列 1:id2 */
 CHECK(tile_focus_column(l,-1)==TILE_OK&&l->workspaces[0].active==0);
 CHECK(tile_focus_column(l,-1)==TILE_OK&&l->workspaces[0].active==0);
 CHECK(tile_move_column(l,-1)==TILE_DENIED); /* 已在最左 */
 CHECK(tile_move_column(l,1)==TILE_OK&&l->workspaces[0].active==1);
 CHECK(l->workspaces[0].columns[0].windows[0].id==2);
 CHECK(l->workspaces[0].columns[1].windows[0].id==1);
 CHECK(tile_move_column(l,1)==TILE_DENIED);
 CHECK(tile_move_column(l,0)==TILE_OK);

 /* 列宽/窗口高度千分比夹紧 */
 CHECK(tile_set_width(l,100)==TILE_OK&&l->workspaces[0].columns[l->workspaces[0].active].width_permille==600);
 CHECK(tile_set_width(l,10000)==TILE_OK&&l->workspaces[0].columns[l->workspaces[0].active].width_permille==TILE_WIDTH_MAX);
 CHECK(tile_set_width(l,-10000)==TILE_OK&&l->workspaces[0].columns[l->workspaces[0].active].width_permille==TILE_WIDTH_MIN);
 CHECK(tile_set_height(l,10000)==TILE_OK&&l->workspaces[0].columns[l->workspaces[0].active].height_permille==TILE_HEIGHT_MAX);
 CHECK(tile_set_height(l,-10000)==TILE_OK&&l->workspaces[0].columns[l->workspaces[0].active].height_permille==TILE_HEIGHT_MIN);

 /* 工作区切换、循环与隔离 */
 l=fresh();
 CHECK(tile_add(l,&id)==TILE_OK);
 CHECK(tile_focus_workspace(l,1)==TILE_OK&&l->active==1);
 CHECK(!l->workspaces[1].count);
 CHECK(tile_focus_workspace(l,1)==TILE_OK&&l->active==2);
 CHECK(tile_focus_workspace(l,-1)==TILE_OK&&l->active==1);
 CHECK(tile_focus_workspace(l,-2)==TILE_OK&&l->active==3);
 CHECK(tile_add(l,&id)==TILE_OK);
 CHECK(l->workspaces[3].columns[0].count==1);
 CHECK(tile_focus_workspace(l,-3)==TILE_OK&&l->active==0);
 CHECK(l->workspaces[0].columns[0].count==1&&l->workspaces[0].columns[0].windows[0].id!=0);
 CHECK(l->workspaces[3].columns[0].count==1);

 /* 滚动：多列、窄列、夹紧与 scroll_to_focus */
 l=fresh();
 CHECK(tile_add(l,&id)==TILE_OK);CHECK(tile_add(l,&id)==TILE_OK);
 CHECK(tile_expel(l)==TILE_OK);
 CHECK(tile_set_width(l,-750)==TILE_OK); /* 当前列（列 1）宽 320 */
 CHECK(tile_focus_column(l,-1)==TILE_OK);
 CHECK(tile_set_width(l,-750)==TILE_OK); /* 列 0 同样窄列 */
 CHECK(tile_focus_column(l,1)==TILE_OK);
 for(unsigned i=0;i<3;i++){
  CHECK(tile_add(l,&id)==TILE_OK);
  CHECK(tile_add(l,&id)==TILE_OK);
  CHECK(tile_expel(l)==TILE_OK);
 }
 CHECK(l->workspaces[0].count==5);
 for(unsigned i=0;i<5;i++)CHECK(l->workspaces[0].columns[i].width_permille==250);
 /* 总宽 = 2*12 + 5*320 + 4*8 = 1656，视口 1280 → 最大滚动 376 */
 CHECK(tile_scroll_by(l,10000)==TILE_OK&&tile_scroll(l)==376);
 CHECK(tile_scroll_by(l,-10000)==TILE_OK&&tile_scroll(l)==0);
 CHECK(tile_focus_column(l,100)==TILE_OK&&l->workspaces[0].active==4);
 CHECK(tile_scroll_to_focus(l)==TILE_OK&&tile_scroll(l)==376);
 CHECK(tile_layout(l,rects,64,&count)==TILE_OK&&count==8);
 CHECK(rects[0].x==12-376);
 CHECK(rects[7].x==12+(int32_t)(4*328)-376);
 CHECK(rects[7].visible==1);
 CHECK(rects[7].width==320);
 /* 回到首列：滚动回到 0 */
 CHECK(tile_focus_column(l,-100)==TILE_OK&&l->workspaces[0].active==0);
 CHECK(tile_scroll_to_focus(l)==TILE_OK&&tile_scroll(l)==0);

 /* 窗口高度放大后超出视口的窗口 visible=0 */
 l=fresh();
 for(unsigned i=0;i<3;i++)CHECK(tile_add(l,&id)==TILE_OK);
 CHECK(tile_set_height(l,10000)==TILE_OK);
 CHECK(tile_layout(l,rects,64,&count)==TILE_OK&&count==3);
 CHECK(rects[0].visible==1);
 CHECK(rects[2].visible==0);

 /* 列内窗口交换：焦点跟随被交换窗口，边界拒绝且状态不变 */
 l=fresh();
 CHECK(tile_add(l,&id)==TILE_OK&&id==1);
 CHECK(tile_add(l,&id)==TILE_OK&&id==2);
 CHECK(tile_add(l,&id)==TILE_OK&&id==3);
 CHECK(l->workspaces[0].columns[0].active==2&&tile_focused_id(l)==3);
 CHECK(tile_swap_window(l,-1)==TILE_OK);
 CHECK(l->workspaces[0].columns[0].windows[0].id==1);
 CHECK(l->workspaces[0].columns[0].windows[1].id==3);
 CHECK(l->workspaces[0].columns[0].windows[2].id==2);
 CHECK(l->workspaces[0].columns[0].active==1&&tile_focused_id(l)==3);
 CHECK(tile_swap_window(l,-1)==TILE_OK&&l->workspaces[0].columns[0].active==0);
 CHECK(l->workspaces[0].columns[0].windows[0].id==3);
 CHECK(tile_swap_window(l,-1)==TILE_DENIED&&l->workspaces[0].columns[0].active==0);
 CHECK(l->workspaces[0].columns[0].windows[0].id==3);
 CHECK(tile_swap_window(l,0)==TILE_OK&&l->workspaces[0].columns[0].active==0);
 CHECK(tile_swap_window(l,1)==TILE_OK&&l->workspaces[0].columns[0].active==1);
 CHECK(l->workspaces[0].columns[0].windows[0].id==1&&l->workspaces[0].columns[0].windows[1].id==3);
 CHECK(tile_swap_window(l,1)==TILE_OK&&l->workspaces[0].columns[0].active==2);
 CHECK(l->workspaces[0].columns[0].windows[1].id==2&&l->workspaces[0].columns[0].windows[2].id==3);
 CHECK(tile_swap_window(l,1)==TILE_DENIED&&l->workspaces[0].columns[0].active==2);
 CHECK(tile_swap_window(l,-100)==TILE_DENIED&&l->workspaces[0].columns[0].active==2);
 l=fresh();
 CHECK(tile_swap_window(l,1)==TILE_EMPTY);
 CHECK(tile_add(l,&id)==TILE_OK);
 CHECK(tile_swap_window(l,1)==TILE_DENIED&&tile_focused_id(l)==1);

 /* 按 id 聚焦：跨工作区命中、滚动到焦点、未找到不改状态 */
 l=fresh();
 CHECK(tile_add(l,&id)==TILE_OK&&id==1);
 CHECK(tile_focus_workspace(l,2)==TILE_OK&&l->active==2);
 CHECK(tile_add(l,&id)==TILE_OK&&id==2);
 CHECK(tile_focused_id(l)==2);
 CHECK(tile_focus_id(l,1)==TILE_OK&&l->active==0&&tile_focused_id(l)==1);
 CHECK(tile_focus_id(l,2)==TILE_OK&&l->active==2&&tile_focused_id(l)==2);
 {
  TileLayout snapshot=*l;
  CHECK(tile_focus_id(l,999)==TILE_NOT_FOUND);
  CHECK(memcmp(&snapshot,l,sizeof(snapshot))==0);
  CHECK(tile_focus_id(l,0)==TILE_INVALID);
 }
 /* 远列窗口聚焦后滚动偏移随之更新 */
 l=fresh();
 CHECK(tile_add(l,&id)==TILE_OK);
 CHECK(tile_add(l,&id)==TILE_OK);
 CHECK(tile_expel(l)==TILE_OK);
 CHECK(tile_set_width(l,-750)==TILE_OK);
 CHECK(tile_focus_column(l,-1)==TILE_OK);
 CHECK(tile_set_width(l,-750)==TILE_OK);
 for(unsigned i=0;i<3;i++){
  CHECK(tile_add(l,&id)==TILE_OK);
  CHECK(tile_add(l,&id)==TILE_OK);
  CHECK(tile_expel(l)==TILE_OK);
 }
 CHECK(tile_focus_column(l,-100)==TILE_OK&&tile_scroll(l)==0);
 CHECK(tile_focus_id(l,l->workspaces[0].columns[4].windows[0].id)==TILE_OK);
 CHECK(tile_scroll(l)==376);

 /* 单列容量上限：第 9 个窗口拒绝且状态不变 */
 l=fresh();
 for(unsigned i=0;i<TILE_WINDOWS;i++)CHECK(tile_add(l,&id)==TILE_OK);
 {
  uint32_t before=l->workspaces[0].columns[0].count;uint64_t next=l->next_id;
  CHECK(tile_add(l,&id)==TILE_NO_SPACE);
  CHECK(l->workspaces[0].columns[0].count==before&&l->next_id==next);
 }
 /* 列容量上限：第 17 列拒绝且状态不变 */
 l=fresh();
 CHECK(tile_add(l,&id)==TILE_OK);CHECK(tile_add(l,&id)==TILE_OK);
 CHECK(tile_expel(l)==TILE_OK);
 while(l->workspaces[0].count<TILE_COLUMNS){
  CHECK(tile_add(l,&id)==TILE_OK);
  CHECK(tile_expel(l)==TILE_OK);
 }
 CHECK(l->workspaces[0].count==TILE_COLUMNS);
 {
  uint32_t columns=l->workspaces[0].count;uint64_t next=l->next_id;
  CHECK(tile_add(l,&id)==TILE_OK); /* 当前列变 2 窗口 */
  CHECK(tile_expel(l)==TILE_NO_SPACE);
  CHECK(l->workspaces[0].count==columns&&l->next_id==next+1);
 }

 printf("TILING PASS: %u checks, workspaces/columns/windows, scroll, focus and capacity\n",checks);
 return 0;
}
