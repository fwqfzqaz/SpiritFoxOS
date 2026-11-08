#include "tiling.h"
/* 布局核心实现：所有状态位于 TileLayout 内，固定数组、无动态分配。
 * 焦点索引与滚动偏移在任何变更后都会夹紧到合法范围。 */
static TileWorkspace *workspace(TileLayout*l){return &l->workspaces[l->active];}

static uint32_t clamp_u32(uint32_t v,uint32_t lo,uint32_t hi){return v<lo?lo:(v>hi?hi:v);}

/* 列宽：视口宽度按千分比缩放，至少 1 像素。 */
static uint32_t column_width(const TileLayout*l,const TileColumn*c){
 uint64_t w=(uint64_t)l->viewport_width*c->width_permille/1000u;
 return w? (uint32_t)w : 1u;
}

/* 内容总宽（含左右外边距与列间距），用于滚动上限。 */
static uint64_t content_width(const TileLayout*l,const TileWorkspace*w){
 if(!w->count)return 0;
 uint64_t total=2ull*l->outer_gap;
 for(uint32_t i=0;i<w->count;i++){
  total+=column_width(l,&w->columns[i]);
  if(i+1<w->count)total+=l->gap;
 }
 return total;
}

static void clamp_scroll(TileLayout*l,TileWorkspace*w){
 uint64_t total=content_width(l,w);
 int64_t max=total>(uint64_t)l->viewport_width? (int64_t)(total-l->viewport_width):0;
 if(w->scroll<0)w->scroll=0;
 if((int64_t)w->scroll>max)w->scroll=(int32_t)max;
}

static void clamp_workspace(TileWorkspace*w){
 if(!w->count){w->active=0;w->scroll=0;return;}
 if(w->active>=w->count)w->active=w->count-1;
 TileColumn*c=&w->columns[w->active];
 if(!c->count){c->active=0;return;}
 if(c->active>=c->count)c->active=c->count-1;
}

/* 删除空列并修正焦点列；调用者保证窗口计数已更新。 */
static void drop_empty_column(TileLayout*l,TileWorkspace*w){
 for(;;){
  if(!w->count){w->active=0;break;}
  if(w->active>=w->count)w->active=w->count-1;
  if(w->columns[w->active].count)break;
  for(uint32_t i=w->active+1;i<w->count;i++)w->columns[i-1]=w->columns[i];
  w->count--;
 }
 clamp_workspace(w);
 clamp_scroll(l,w);
}

void tile_init(TileLayout*l,uint32_t vw,uint32_t vh,uint32_t bar,uint32_t outer,uint32_t gap){
 if(!l)return;
 uint8_t*bytes=(uint8_t*)l;
 for(uint32_t i=0;i<sizeof(*l);i++)bytes[i]=0;
 l->viewport_width=vw;l->viewport_height=vh;l->bar_height=bar;
 l->outer_gap=outer;l->gap=gap;l->next_id=1;l->active=0;
 /* 默认 500‰：列占视口一半宽，符合 niri 的典型双列观感。 */
 for(uint32_t i=0;i<TILE_WORKSPACES;i++)
  for(uint32_t j=0;j<TILE_COLUMNS;j++){
   l->workspaces[i].columns[j].width_permille=500;
   l->workspaces[i].columns[j].height_permille=1000;
  }
}

int tile_add(TileLayout*l,uint64_t*out_id){
 if(!l||!out_id)return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 if(!w->count){
  if(TILE_COLUMNS<1)return TILE_NO_SPACE;
  w->columns[0].count=1;w->columns[0].active=0;
  w->columns[0].width_permille=500;w->columns[0].height_permille=1000;
  w->columns[0].windows[0].id=l->next_id++;
  w->count=1;w->active=0;*out_id=w->columns[0].windows[0].id;
  clamp_scroll(l,w);return TILE_OK;
 }
 TileColumn*c=&w->columns[w->active];
 if(c->count>=TILE_WINDOWS)return TILE_NO_SPACE;
 uint32_t at=c->count?c->active+1:0;
 for(uint32_t i=c->count;i>at;i--)c->windows[i]=c->windows[i-1];
 c->windows[at].id=l->next_id++;c->count++;c->active=at;
 *out_id=c->windows[at].id;clamp_scroll(l,w);return TILE_OK;
}

int tile_close_focused(TileLayout*l){
 if(!l)return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 if(!w->count)return TILE_EMPTY;
 TileColumn*c=&w->columns[w->active];
 if(!c->count)return TILE_EMPTY;
 for(uint32_t i=c->active;i+1<c->count;i++)c->windows[i]=c->windows[i+1];
 c->count--;
 if(c->active>=c->count)c->active=c->count?c->count-1:0;
 drop_empty_column(l,w);return TILE_OK;
}

int tile_consume(TileLayout*l){
 if(!l)return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 if(w->active==0||!w->count)return TILE_DENIED;
 TileColumn*src=&w->columns[w->active],*dst=&w->columns[w->active-1];
 if(!src->count)return TILE_EMPTY;
 if(dst->count>=TILE_WINDOWS)return TILE_NO_SPACE;
 uint64_t id=src->windows[src->active].id;
 dst->windows[dst->count].id=id;dst->count++;dst->active=dst->count-1;
 for(uint32_t i=src->active;i+1<src->count;i++)src->windows[i]=src->windows[i+1];
 src->count--;
 if(!src->count){
  for(uint32_t i=w->active+1;i<w->count;i++)w->columns[i-1]=w->columns[i];
  w->count--;w->active--;
 }else{
  if(src->active>=src->count)src->active=src->count-1;
  w->active--;
 }
 clamp_workspace(w);clamp_scroll(l,w);return TILE_OK;
}

int tile_expel(TileLayout*l){
 if(!l)return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 if(!w->count)return TILE_EMPTY;
 TileColumn*c=&w->columns[w->active];
 if(c->count<2)return TILE_DENIED;
 if(w->count>=TILE_COLUMNS)return TILE_NO_SPACE;
 uint32_t at=w->active+1;
 for(uint32_t i=w->count;i>at;i--)w->columns[i]=w->columns[i-1];
 w->count++;
 TileColumn*n=&w->columns[at];
 uint8_t*bytes=(uint8_t*)n;for(uint32_t i=0;i<sizeof(*n);i++)bytes[i]=0;
 n->count=1;n->active=0;n->width_permille=c->width_permille;n->height_permille=c->height_permille;
 n->windows[0].id=c->windows[c->active].id;
 for(uint32_t i=c->active;i+1<c->count;i++)c->windows[i]=c->windows[i+1];
 c->count--;
 if(c->active>=c->count)c->active=c->count-1;
 w->active=at;clamp_workspace(w);clamp_scroll(l,w);return TILE_OK;
}

int tile_move_column(TileLayout*l,int delta){
 if(!l)return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 if(!w->count)return TILE_EMPTY;
 if(!delta)return TILE_OK;
 int64_t target=(int64_t)w->active+delta;
 if(target<0||target>=(int64_t)w->count)return TILE_DENIED;
 TileColumn tmp=w->columns[w->active];
 w->columns[w->active]=w->columns[target];
 w->columns[target]=tmp;
 w->active=(uint32_t)target;clamp_scroll(l,w);return TILE_OK;
}

int tile_focus_window(TileLayout*l,int delta){
 if(!l)return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 if(!w->count)return TILE_EMPTY;
 TileColumn*c=&w->columns[w->active];
 if(!c->count)return TILE_EMPTY;
 int64_t target=(int64_t)c->active+delta;
 if(target<0)target=0;
 if(target>=(int64_t)c->count)target=(int64_t)c->count-1;
 c->active=(uint32_t)target;return TILE_OK;
}

int tile_swap_window(TileLayout*l,int delta){
 if(!l)return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 if(!w->count)return TILE_EMPTY;
 TileColumn*c=&w->columns[w->active];
 if(!c->count)return TILE_EMPTY;
 if(!delta)return TILE_OK;
 int64_t target=(int64_t)c->active+delta;
 if(target<0||target>=(int64_t)c->count)return TILE_DENIED;
 TileWindow tmp=c->windows[c->active];
 c->windows[c->active]=c->windows[target];
 c->windows[target]=tmp;
 c->active=(uint32_t)target;return TILE_OK;
}

int tile_focus_column(TileLayout*l,int delta){
 if(!l)return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 if(!w->count)return TILE_EMPTY;
 int64_t target=(int64_t)w->active+delta;
 if(target<0)target=0;
 if(target>=(int64_t)w->count)target=(int64_t)w->count-1;
 w->active=(uint32_t)target;clamp_scroll(l,w);return TILE_OK;
}

int tile_focus_workspace(TileLayout*l,int delta){
 if(!l)return TILE_INVALID;
 int32_t active=(int32_t)l->active+delta;
 active%=TILE_WORKSPACES;if(active<0)active+=TILE_WORKSPACES;
 l->active=(uint32_t)active;return TILE_OK;
}

int tile_set_width(TileLayout*l,int delta){
 if(!l)return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 if(!w->count)return TILE_EMPTY;
 TileColumn*c=&w->columns[w->active];
 int32_t v=(int32_t)c->width_permille+delta;
 c->width_permille=clamp_u32((uint32_t)(v<0?0:v),TILE_WIDTH_MIN,TILE_WIDTH_MAX);
 clamp_scroll(l,w);tile_scroll_to_focus(l);return TILE_OK;
}

int tile_set_height(TileLayout*l,int delta){
 if(!l)return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 if(!w->count)return TILE_EMPTY;
 TileColumn*c=&w->columns[w->active];
 int32_t v=(int32_t)c->height_permille+delta;
 c->height_permille=clamp_u32((uint32_t)(v<0?0:v),TILE_HEIGHT_MIN,TILE_HEIGHT_MAX);
 return TILE_OK;
}

int tile_scroll_by(TileLayout*l,int delta){
 if(!l)return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 int64_t v=(int64_t)w->scroll+delta;
 if(v<0)v=0;
 if(v>INT32_MAX)v=INT32_MAX;
 w->scroll=(int32_t)v;clamp_scroll(l,w);return TILE_OK;
}

int tile_scroll_to_focus(TileLayout*l){
 if(!l)return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 if(!w->count)return TILE_EMPTY;
 int64_t x=(int64_t)l->outer_gap;
 for(uint32_t i=0;i<w->active;i++)x+=(int64_t)column_width(l,&w->columns[i])+l->gap;
 int64_t width=column_width(l,&w->columns[w->active]);
 int64_t scroll=w->scroll;
 if(width+2ll*(int64_t)l->outer_gap>(int64_t)l->viewport_width){
  /* 列比视口更宽：左对齐（含外边距），不做右侧约束。 */
  scroll=x-(int64_t)l->outer_gap;
 }else if(x-scroll<(int64_t)l->outer_gap){
  scroll=x-(int64_t)l->outer_gap;
 }else if(x-scroll+width+(int64_t)l->outer_gap>(int64_t)l->viewport_width){
  scroll=x+width+(int64_t)l->outer_gap-(int64_t)l->viewport_width;
 }
 if(scroll<0)scroll=0;
 if(scroll>INT32_MAX)scroll=INT32_MAX;
 w->scroll=(int32_t)scroll;clamp_scroll(l,w);return TILE_OK;
}

/* 计算活动工作区几何。返回窗口总数；容量不足时不写 out。 */
int tile_layout(TileLayout*l,TileRect*out,uint32_t capacity,uint32_t*count){
 if(!l||!count||(!out&&capacity))return TILE_INVALID;
 TileWorkspace*w=workspace(l);
 uint32_t total=0;
 for(uint32_t i=0;i<w->count;i++)total+=w->columns[i].count;
 *count=total;
 if(!total)return TILE_EMPTY;
 if(capacity<total)return TILE_NO_SPACE;
 uint32_t content_h=l->viewport_height>l->bar_height?l->viewport_height-l->bar_height:0;
 int64_t x=(int64_t)l->outer_gap-(int64_t)w->scroll;
 uint32_t n=0;
 for(uint32_t ci=0;ci<w->count;ci++){
  TileColumn*c=&w->columns[ci];
  uint32_t width=column_width(l,c);
  int64_t y=l->bar_height;
  if(c->count){
   int64_t base=((int64_t)content_h-(int64_t)(c->count-1)*(int64_t)l->gap)/(int64_t)c->count;
   if(base<1)base=1;
   int64_t height=base*(int64_t)c->height_permille/1000;
   if(height<1)height=1;
   for(uint32_t wi=0;wi<c->count;wi++){
    TileRect*r=&out[n++];
    r->x=(int32_t)x;r->y=(int32_t)(y>INT32_MAX?INT32_MAX:y);
    r->width=width;r->height=(uint32_t)height;
    r->visible=(x<(int64_t)l->viewport_width&&x+(int64_t)width>0&&
                y<(int64_t)l->viewport_height&&y+(int64_t)height>(int64_t)l->bar_height)?1u:0u;
    y+=height+l->gap;
   }
  }
  x+=(int64_t)width+l->gap;
 }
 return TILE_OK;
}

uint64_t tile_focused_id(const TileLayout*l){
 if(!l)return 0;
 const TileWorkspace*w=&l->workspaces[l->active];
 if(!w->count)return 0;
 const TileColumn*c=&w->columns[w->active];
 if(!c->count)return 0;
 return c->windows[c->active].id;
}

int tile_focus_id(TileLayout*l,uint64_t id){
 if(!l||!id)return TILE_INVALID;
 for(uint32_t wi=0;wi<TILE_WORKSPACES;wi++){
  TileWorkspace*w=&l->workspaces[wi];
  for(uint32_t ci=0;ci<w->count;ci++){
   TileColumn*c=&w->columns[ci];
   for(uint32_t n=0;n<c->count;n++){
    if(c->windows[n].id!=id)continue;
    l->active=wi;w->active=ci;c->active=n;
    return tile_scroll_to_focus(l);
   }
  }
 }
 return TILE_NOT_FOUND;
}

uint32_t tile_scroll(const TileLayout*l){
 if(!l)return 0;
 return (uint32_t)l->workspaces[l->active].scroll;
}
