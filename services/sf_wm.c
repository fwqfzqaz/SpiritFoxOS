/* sf-wm：ring 3 的单一有界合成器。映射固件 framebuffer，按类 niri 的
 * 可滚动平铺模型绘制工作区/列/窗口。所有操作都由上栏按钮与指针命中触发，
 * 不再接受布局快捷键；只有 SFK 输入行保留文本编辑。 */
#include "libsf.h"
#include "tiling.h"
#include "launcher.h"
#include "sfk.h"
#define WM_CAPACITY (TILE_COLUMNS*TILE_WINDOWS)
#define WM_BAR_HEIGHT 32u
#define WM_OUTER_GAP 12u
#define WM_GAP 8u
#define WM_BAR_BUTTON_H 20
#define WM_SEARCH_BUTTON_W 132
#define WM_SHELL_BUTTON_W 96
enum { WM_BG=0x00101820u, WM_BAR=0x00202830u, WM_TAB=0x00303840u,
       WM_TAB_ACTIVE=0x00406080u, WM_WIN=0x00384858u, WM_WIN_ALT=0x00405868u,
       WM_BORDER=0x00608090u, WM_FOCUS=0x00e0a040u, WM_ACCENT=0x007090a0u,
       WM_TEXT=0x00e8f0f8u, WM_TEXT_DIM=0x00a8bcc8u,
       WM_PANEL=0x00182028u, WM_SELECT=0x00406080u, WM_BLACK=0x00000000u };
static SfFbInfo info;
static volatile uint32_t *fb;
static uint32_t stride;
static TileLayout layout;
static Launcher launcher;
static int search_open;
static Sfk shell;
static int shell_open;
static int32_t cursor_x,cursor_y;
static uint8_t cursor_buttons;
static int redraw_needed=1;

static uint32_t checksum(void){
 uint32_t hash=2166136261u;uint32_t words=stride*info.height;
 for(uint32_t i=0;i<words;i++){hash^=fb[i];hash*=16777619u;}
 return hash;
}
/* 状态行在单次 SYS_LOG 中写出：多任务分片日志曾与 `sf-init` 的换行
 * 交错，导致 `WM: fb=<宽>x<高>` 被拆行、测试正则无法匹配。 */
static char* number(uint32_t v,char*out);
typedef struct {char b[112];uint32_t n;} Stat;
static void stat_add(Stat*s,const char*t){
 while(*t&&s->n+1u<sizeof(s->b))s->b[s->n++]=*t++;
 s->b[s->n]=0;
}
static void stat_num(Stat*s,uint32_t v){char t[12];stat_add(s,number(v,t));}
static void stat_hex(Stat*s,uint32_t v){
 char t[9];for(unsigned i=0;i<8;i++)t[i]="0123456789abcdef"[(v>>(28-4*i))&15];t[8]=0;stat_add(s,t);
}
static void log_state(void){
 uint32_t cols=0,wins=0;
 TileWorkspace*w=&layout.workspaces[layout.active];
 for(uint32_t i=0;i<w->count;i++){cols++;wins+=w->columns[i].count;}
 Stat s;s.n=0;s.b[0]=0;
 stat_add(&s,"WM: fb=");stat_num(&s,info.width);stat_add(&s,"x");stat_num(&s,info.height);
 stat_add(&s," bpp=");stat_num(&s,info.bpp);
 stat_add(&s," ws=");stat_num(&s,layout.active+1);
 stat_add(&s," col=");stat_num(&s,cols);
 stat_add(&s," win=");stat_num(&s,wins);
 stat_add(&s," scroll=");stat_num(&s,(uint32_t)tile_scroll(&layout));
 stat_add(&s," checksum=");stat_hex(&s,checksum());
 stat_add(&s,"\n");
 sf_log(s.b);
}
static void fill(int32_t x,int32_t y,int32_t w,int32_t h,uint32_t color){
 int64_t x0=x,y0=y,x1=(int64_t)x+w,y1=(int64_t)y+h;
 if(x0<0)x0=0;if(y0<0)y0=0;
 if(x1>(int64_t)info.width)x1=info.width;
 if(y1>(int64_t)info.height)y1=info.height;
 if(x0>=x1||y0>=y1)return;
 for(int64_t row=y0;row<y1;row++)for(int64_t col=x0;col<x1;col++)fb[(uint32_t)row*stride+(uint32_t)col]=color;
}
/* 5x7 位图字体：每字形 7 行，每行低 5 位是像素，最高位为最左。
 * 覆盖启动器需要的 A-Z、0-9 与常用标点；小写按大写绘制。 */
static const uint8_t font_rows[][7]={
 {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
 {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
 {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
 {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
 {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
 {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
 {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
 {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
 {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
 {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
 {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
 {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
 {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
 {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
 {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
 {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
 {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
 {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
 {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
 {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
 {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
 {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
 {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
 {0x11,0x13,0x15,0x15,0x19,0x11,0x11}, /* N */
 {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
 {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
 {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
 {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
 {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
 {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
 {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
 {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
 {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* W */
 {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
 {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
 {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
 {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
 {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /* _ */
 {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, /* . */
 {0x01,0x01,0x02,0x04,0x08,0x10,0x10}, /* / */
 {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, /* : */
 {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* < */
 {0x08,0x04,0x02,0x01,0x02,0x04,0x08}  /* > */
};
static int glyph(char c){
 if(c>='0'&&c<='9')return (int)(c-'0');
 if(c>='A'&&c<='Z')return 10+(int)(c-'A');
 if(c>='a'&&c<='z')return 10+(int)(c-'a');
 switch(c){case '-':return 36;case '_':return 37;case '.':return 38;
  case '/':return 39;case ':':return 40;case '<':return 41;case '>':return 42;default:return -1;}
}
/* 返回文本结束后的 x 坐标；未知字符按空格推进。 */
static int32_t text(int32_t x,int32_t y,const char*s,uint32_t scale,uint32_t color){
 for(;*s;s++){
  int gi=glyph(*s);
  if(gi<0){x+=6*(int32_t)scale;continue;}
  const uint8_t*rows=font_rows[gi];
  for(uint32_t r=0;r<7;r++){
   uint8_t bits=rows[r];
   for(uint32_t c=0;c<5;c++)if(bits&(0x10u>>c))
    fill(x+(int32_t)c*(int32_t)scale,y+(int32_t)r*(int32_t)scale,scale,scale,color);
  }
  x+=6*(int32_t)scale;
 }
 return x;
}
static char* number(uint32_t v,char*out){
 char tmp[12];unsigned n=0;
 do{tmp[n++]=(char)('0'+v%10u);v/=10u;}while(v&&n<12u);
 for(unsigned i=0;i<n;i++)out[i]=tmp[n-1-i];
 out[n]=0;return out;
}
/* 历史行拼装：固定 64 字节，超出部分截断，避免为 shell 引入格式化器。 */
typedef struct {char b[SFK_HISTORY_BYTES];uint32_t n;} Line;
static void line_reset(Line*l){l->n=0;l->b[0]=0;}
static void line_add(Line*l,const char*s){
 if(!s)return;
 while(*s&&l->n+1u<SFK_HISTORY_BYTES)l->b[l->n++]=*s++;
 l->b[l->n]=0;
}
static void line_num(Line*l,uint32_t v){char t[12];line_add(l,number(v,t));}
/* 固定半径的圆角矩形填充；半径表覆盖窗口/焦点环/页签/徽标。 */
static const uint8_t*inset_table(uint32_t r){
 static const uint8_t t4[4]={2,1,1,0},t5[5]={2,2,1,0,0},t7[7]={3,2,2,1,1,0,0},
  t8[8]={4,3,2,1,1,0,0,0},t10[10]={5,4,3,2,2,1,1,0,0,0};
 switch(r){case 4:return t4;case 5:return t5;case 7:return t7;case 8:return t8;case 10:return t10;default:return 0;}
}
static void round_fill(int32_t x,int32_t y,int32_t w,int32_t h,uint32_t r,uint32_t color){
 if(w<=0||h<=0)return;
 const uint8_t*inset=inset_table(r);
 if(!inset||(int32_t)r*2>w||(int32_t)r*2>h){fill(x,y,w,h,color);return;}
 for(uint32_t j=0;j<r;j++){
  int32_t a=(int32_t)inset[j];
  fill(x+a,y+(int32_t)j,w-2*a,1,color);
  fill(x+a,y+h-1-(int32_t)j,w-2*a,1,color);
 }
 fill(x,y+(int32_t)r,w,h-2*(int32_t)r,color);
}
/* niri 风格的窗口徽标：小圆角色块内标出窗口 id。 */
static void badge(int32_t x,int32_t y,uint64_t id){
 char num[12];number((uint32_t)id,num);uint32_t len=0;while(num[len])len++;
 int32_t w=12+6*(int32_t)(1u+len);
 round_fill(x,y,w,16,4,WM_ACCENT);
 text(x+6,y+4,"W",1,WM_TEXT);
 text(x+12,y+4,num,1,WM_TEXT);
}
static int32_t bar_search_x(void){return(int32_t)info.width-8-(int32_t)WM_SEARCH_BUTTON_W;}
static int32_t bar_shell_x(void){return bar_search_x()-8-(int32_t)WM_SHELL_BUTTON_W;}
static int bar_button_hit(int32_t x,int32_t y,int32_t bx,int32_t width){
 return x>=bx&&x<bx+width&&y>=6&&y<6+WM_BAR_BUTTON_H;
}
/* niri 风格上栏：左侧工作区，右侧 SHELL/SEARCH 两个可点击按钮。 */
static void draw_bar(void){
 fill(0,0,(int32_t)info.width,(int32_t)WM_BAR_HEIGHT,WM_BAR);
 uint32_t cols=0,wins=0;
 TileWorkspace*w=&layout.workspaces[layout.active];
 for(uint32_t i=0;i<w->count;i++){cols++;wins+=w->columns[i].count;}
 for(uint32_t i=0;i<TILE_WORKSPACES;i++){
  int32_t x=8+(int32_t)i*48;
  int hit=bar_button_hit(cursor_x,cursor_y,x,40);
  round_fill(x,6,40,WM_BAR_BUTTON_H,4,i==layout.active?WM_TAB_ACTIVE:hit?WM_SELECT:WM_TAB);
  char label[2]={(char)('1'+i),0};
  text(x+14,9,label,2,i==layout.active?WM_TEXT:WM_TEXT_DIM);
 }
 char buf[12];
 int32_t x=8+(int32_t)TILE_WORKSPACES*48+12;
 x=text(x,9,"WS ",2,WM_TEXT_DIM);
 x=text(x,9,number(layout.active+1,buf),2,WM_TEXT);
 x=text(x,9,"  COL ",2,WM_TEXT_DIM);
 x=text(x,9,number(cols,buf),2,WM_TEXT);
 x=text(x,9,"  WIN ",2,WM_TEXT_DIM);
 text(x,9,number(wins,buf),2,WM_TEXT);
 int32_t hx=bar_shell_x(),sx=bar_search_x();
 round_fill(hx,6,WM_SHELL_BUTTON_W,WM_BAR_BUTTON_H,4,
  shell_open?WM_TAB_ACTIVE:bar_button_hit(cursor_x,cursor_y,hx,WM_SHELL_BUTTON_W)?WM_SELECT:WM_TAB);
 text(hx+18,9,"SHELL",2,WM_TEXT);
 round_fill(sx,6,WM_SEARCH_BUTTON_W,WM_BAR_BUTTON_H,4,
  search_open?WM_TAB_ACTIVE:bar_button_hit(cursor_x,cursor_y,sx,WM_SEARCH_BUTTON_W)?WM_SELECT:WM_TAB);
 text(sx+30,9,"SEARCH",2,WM_TEXT);
}
static void draw_frame(const TileRect*r,uint64_t id,uint64_t focus){
 int32_t x=r->x,y=r->y,w=(int32_t)r->width,h=(int32_t)r->height;
 uint32_t body=(id&1u)?WM_WIN:WM_WIN_ALT;
 if(id==focus)round_fill(x-3,y-3,w+6,h+6,10,WM_FOCUS);
 round_fill(x,y,w,h,8,WM_BORDER);
 round_fill(x+1,y+1,w-2,h-2,7,body);
 badge(x+8,y+8,id);
}
/* 叠加层几何：绘制与命中测试共用，避免两处坐标漂移。 */
static void search_geometry(int32_t*px,int32_t*py,int32_t*pw,int32_t*ph){
 *pw=620;*ph=420;
 *px=((int32_t)info.width-*pw)/2;*py=((int32_t)info.height-*ph)/2;
 if(*px<0)*px=0;
 if(*py<(int32_t)WM_BAR_HEIGHT)*py=(int32_t)WM_BAR_HEIGHT;
 if(*px+*pw>(int32_t)info.width)*pw=(int32_t)info.width-*px;
 if(*py+*ph>(int32_t)info.height)*ph=(int32_t)info.height-*py;
}
static int32_t search_row_y(int32_t py,uint32_t index){return py+92+(int32_t)index*36;}
/* 命中结果：>=0 为行号，-1 为面板内空白，-2 为面板外。 */
static int search_index_at(int32_t x,int32_t y){
 int32_t px,py,pw,ph;search_geometry(&px,&py,&pw,&ph);
 if(pw<200||ph<160)return -1;
 if(x<px||x>=px+pw||y<py||y>=py+ph)return -2;
 for(uint32_t i=0;i<launcher.count&&i<8u;i++){
  int32_t row=search_row_y(py,i);
  if(y>=row-6&&y<row+26&&x>=px+14&&x<px+pw-14)return(int)i;
 }
 return -1;
}
/* 应用搜索面板：直接列出目录条目，点击行即可打开，无键盘查询。 */
static void draw_search(void){
 int32_t px,py,pw,ph;search_geometry(&px,&py,&pw,&ph);
 if(pw<200||ph<160)return;
 round_fill(px-3,py-3,pw+6,ph+6,10,WM_ACCENT);
 round_fill(px,py,pw,ph,8,WM_PANEL);
 text(px+20,py+18,"SEARCH APPS",2,WM_TEXT);
 text(px+20,py+46,"CLICK AN APP TO OPEN",1,WM_TEXT_DIM);
 fill(px+20,py+64,pw-40,2,WM_ACCENT);
 uint32_t rows=launcher.count;if(rows>8u)rows=8u;
 if(!rows)text(px+24,search_row_y(py,0),"NO APPS",2,WM_TEXT_DIM);
 int hover=search_index_at(cursor_x,cursor_y);
 for(uint32_t i=0;i<rows;i++){
  int32_t row=search_row_y(py,i);
  if((int)i==hover)round_fill(px+14,row-6,pw-28,32,4,WM_SELECT);
  text(px+28,row,launcher.entries[i].title,2,WM_TEXT);
  if(launcher.entries[i].window){
   Line l;line_reset(&l);line_add(&l,"W");
   line_num(&l,(uint32_t)launcher.entries[i].window);
   text(px+pw-24-6*(int32_t)l.n,row,l.b,2,WM_TEXT_DIM);
  }
 }
 text(px+20,py+ph-28,"CLICK OUTSIDE TO CLOSE",1,WM_TEXT_DIM);
}
/* SFK 图形 shell：近似全屏的圆角面板，历史行 + 输入行 + 命令提示。
 * 面板尺寸随 framebuffer 夹紧；过小的显示不做叠加层以免错位。 */
static void shell_geometry(int32_t*px,int32_t*py,int32_t*pw,int32_t*ph){
 *pw=1216;*ph=720;
 *px=((int32_t)info.width-*pw)/2;*py=((int32_t)info.height-*ph)/2;
 if(*px<0)*px=0;
 if(*py<(int32_t)WM_BAR_HEIGHT)*py=(int32_t)WM_BAR_HEIGHT;
 if(*px+*pw>(int32_t)info.width)*pw=(int32_t)info.width-*px;
 if(*py+*ph>(int32_t)info.height)*ph=(int32_t)info.height-*py;
}
static int shell_close_hit(int32_t x,int32_t y){
 int32_t px,py,pw,ph;shell_geometry(&px,&py,&pw,&ph);
 if(pw<640||ph<480)return 0;
 return x>=px+pw-44&&x<px+pw-16&&y>=py+14&&y<py+40;
}
static void draw_shell(void){
 int32_t px,py,pw,ph;shell_geometry(&px,&py,&pw,&ph);
 if(pw<640||ph<480)return;
 round_fill(px-3,py-3,pw+6,ph+6,10,WM_ACCENT);
 round_fill(px,py,pw,ph,8,WM_PANEL);
 text(px+24,py+20,"SFK // SPIRITFOX GRAPHICAL SHELL",2,WM_TEXT);
 round_fill(px+pw-44,py+14,28,26,4,
  shell_close_hit(cursor_x,cursor_y)?WM_TAB_ACTIVE:WM_SELECT);
 text(px+pw-34,py+18,"X",2,WM_TEXT);
 fill(px+24,py+50,pw-48,2,WM_ACCENT);
 uint32_t lines=sfk_line_count(&shell);
 if(lines>SFK_HISTORY_LINES)lines=SFK_HISTORY_LINES;
 for(uint32_t i=0;i<lines;i++){
  const char*h=sfk_line_at(&shell,i);
  text(px+24,py+70+(int32_t)i*28,h,2,(h[0]=='>')?WM_TEXT_DIM:WM_TEXT);
 }
 round_fill(px+20,py+436,pw-40,44,6,WM_SELECT);
 int32_t x=text(px+32,py+452,"SFK> ",2,WM_TEXT);
 x=text(x,py+452,shell.input,2,WM_TEXT);
 fill(x,py+452,8,14,WM_TEXT);
 text(px+24,py+ph-34,"HELP  APPS  OPEN <APP>  WS <1-4>  SEARCH  EXIT  |  CLICK X TO CLOSE",1,WM_TEXT_DIM);
}
/* 指针：12×12 箭头位图，先画四向黑色描边再覆盖白色填充。
 * 移动时只回存/重画 14×14 光标区域，避免每个指针包整屏重绘造成卡顿与撕裂。 */
static const uint16_t cursor_mask[12]={
 0x800u,0xC00u,0xE00u,0xF00u,0xF80u,0xFC0u,0xFE0u,0xFF0u,0xF80u,0xC00u,0xC00u,0xC00u
};
#define WM_CURSOR_SPAN 14
static uint32_t cursor_bg[WM_CURSOR_SPAN*WM_CURSOR_SPAN];
static int32_t cursor_bg_x,cursor_bg_y;
static int cursor_saved;
static void cursor_restore(void){
 if(!cursor_saved)return;
 cursor_saved=0;
 for(int32_t j=0;j<WM_CURSOR_SPAN;j++)for(int32_t i=0;i<WM_CURSOR_SPAN;i++){
  int32_t x=cursor_bg_x+i,y=cursor_bg_y+j;
  if(x>=0&&y>=0&&x<(int32_t)info.width&&y<(int32_t)info.height)
   fb[(uint32_t)y*stride+(uint32_t)x]=cursor_bg[(uint32_t)(j*WM_CURSOR_SPAN+i)];
 }
}
static void cursor_paint(void){
 cursor_bg_x=cursor_x-1;cursor_bg_y=cursor_y-1;
 for(int32_t j=0;j<WM_CURSOR_SPAN;j++)for(int32_t i=0;i<WM_CURSOR_SPAN;i++){
  int32_t x=cursor_bg_x+i,y=cursor_bg_y+j;
  uint32_t value=0;
  if(x>=0&&y>=0&&x<(int32_t)info.width&&y<(int32_t)info.height)
   value=fb[(uint32_t)y*stride+(uint32_t)x];
  cursor_bg[(uint32_t)(j*WM_CURSOR_SPAN+i)]=value;
 }
 for(int pass=0;pass<5;pass++){
  int32_t ox=pass==0?-1:pass==1?1:0;
  int32_t oy=pass==2?-1:pass==3?1:0;
  uint32_t color=pass<4?WM_BLACK:WM_TEXT;
  for(uint32_t r=0;r<12;r++)for(uint32_t c=0;c<12;c++){
   if(!(cursor_mask[r]&(0x800u>>c)))continue;
   int32_t x=cursor_x+ox+(int32_t)c,y=cursor_y+oy+(int32_t)r;
   if(x>=0&&y>=0&&x<(int32_t)info.width&&y<(int32_t)info.height)
    fb[(uint32_t)y*stride+(uint32_t)x]=color;
  }
 }
 cursor_saved=1;
}
static void draw(void){
 fill(0,0,(int32_t)info.width,(int32_t)info.height,WM_BG);
 draw_bar();
 int32_t content_height=(int32_t)(info.height>WM_BAR_HEIGHT?info.height-WM_BAR_HEIGHT:0);
 if(content_height){
  TileRect rects[WM_CAPACITY];uint32_t count=0;
  if(tile_layout(&layout,rects,WM_CAPACITY,&count)==TILE_OK){
   TileWorkspace*w=&layout.workspaces[layout.active];
   uint64_t focus=tile_focused_id(&layout);
   uint32_t index=0;
   for(uint32_t ci=0;ci<w->count;ci++){
    TileColumn*c=&w->columns[ci];
    for(uint32_t wi=0;wi<c->count&&index<count;wi++){
     if(rects[index].visible)draw_frame(&rects[index],c->windows[wi].id,focus);
     index++;
    }
   }
  }
 }
 if(search_open)draw_search();
 if(shell_open)draw_shell();
 cursor_saved=0;  /* 整屏重绘已覆盖旧光标区域 */
 cursor_paint();
}
static void mark_drawn(void){
 redraw_needed=0;
 log_state();
}
/* 列宽预设循环（niri 的 1/3、1/2、2/3、全宽）。 */
static int width_preset(TileLayout*l){
 TileWorkspace*w=&l->workspaces[l->active];
 if(!w->count)return 0;
 uint32_t cur=w->columns[w->active].width_permille;
 static const uint32_t presets[]={333u,500u,667u,1000u};
 uint32_t next=presets[0];
 for(unsigned i=0;i<sizeof(presets)/sizeof(presets[0]);i++)if(presets[i]>cur){next=presets[i];break;}
 if(next==cur)next=presets[0];
 int r=tile_set_width(l,(int)next-(int)cur);
 return r==TILE_OK&&w->columns[w->active].width_permille!=cur;
}
static void click_log(const char*name){
 Line l;line_reset(&l);line_add(&l,"WM: click ");line_add(&l,name);line_add(&l,"\n");
 sf_log(l.b);
}
/* 激活目录条目：已有窗口则跨工作区聚焦，否则新建窗口并回写目录。 */
static void open_entry(uint32_t index){
 if(index>=launcher.count)return;
 LauncherEntry*e=&launcher.entries[index];
 Line l;line_reset(&l);line_add(&l,"WM: click APP ");line_add(&l,e->title);line_add(&l,"\n");
 if(!(e->window&&tile_focus_id(&layout,e->window)==TILE_OK)){
  uint64_t id=0;
  if(tile_add(&layout,&id)==TILE_OK)e->window=id;
 }
 sf_log(l.b);
}
/* 内容区命中：返回窗口 id；未命中返回 0。 */
static uint64_t window_hit(int32_t x,int32_t y){
 TileRect rects[WM_CAPACITY];uint32_t count=0;
 if(tile_layout(&layout,rects,WM_CAPACITY,&count)!=TILE_OK)return 0;
 TileWorkspace*w=&layout.workspaces[layout.active];
 uint32_t index=0;
 for(uint32_t ci=0;ci<w->count;ci++){
  TileColumn*c=&w->columns[ci];
  for(uint32_t wi=0;wi<c->count;wi++){
   if(index>=count)return 0;
   TileRect*r=&rects[index++];
   if(r->visible&&x>=r->x&&x<r->x+(int32_t)r->width&&y>=r->y&&y<r->y+(int32_t)r->height)
    return c->windows[wi].id;
  }
 }
 return 0;
}
/* 指针按下沿：叠加层优先，其次上栏按钮，最后窗口聚焦。 */
static void pointer_press(int32_t x,int32_t y){
 if(shell_open){
  if(shell_close_hit(x,y)){shell_open=0;click_log("CLOSE");}
  return;
 }
 if(search_open){
  int hit=search_index_at(x,y);
  if(hit>=0){open_entry((uint32_t)hit);search_open=0;}
  else if(hit==-1)click_log("NONE");
  else {search_open=0;click_log("CLOSE");}
  return;
 }
 if(y<(int32_t)WM_BAR_HEIGHT){
  for(uint32_t i=0;i<TILE_WORKSPACES;i++){
   if(bar_button_hit(x,y,8+(int32_t)i*48,40)){
    tile_focus_workspace(&layout,(int)i-(int)layout.active);
    Line l;line_reset(&l);line_add(&l,"WM: click WS");line_num(&l,i+1);line_add(&l,"\n");
    sf_log(l.b);return;
   }
  }
  if(bar_button_hit(x,y,bar_search_x(),WM_SEARCH_BUTTON_W)){search_open=1;click_log("SEARCH");return;}
  if(bar_button_hit(x,y,bar_shell_x(),WM_SHELL_BUTTON_W)){shell_open=1;click_log("SHELL");return;}
  click_log("BAR");return;
 }
 uint64_t id=window_hit(x,y);
 if(id&&tile_focus_id(&layout,id)==TILE_OK){
  Line l;line_reset(&l);line_add(&l,"WM: click FOCUS W");line_num(&l,(uint32_t)id);line_add(&l,"\n");
  sf_log(l.b);
 }else click_log("DESKTOP");
}
/* 悬停签名：只有跨入/跨出可交互元素或搜索行时才需要重绘场景。 */
static uint32_t hover_state(void){
 uint32_t hover=0;
 for(uint32_t i=0;i<TILE_WORKSPACES;i++)
  if(bar_button_hit(cursor_x,cursor_y,8+(int32_t)i*48,40))hover=i+1u;
 if(bar_button_hit(cursor_x,cursor_y,bar_shell_x(),WM_SHELL_BUTTON_W))hover=6u;
 if(bar_button_hit(cursor_x,cursor_y,bar_search_x(),WM_SEARCH_BUTTON_W))hover=7u;
 if(search_open){
  int row=search_index_at(cursor_x,cursor_y);
  if(row>=0)hover=8u+(uint32_t)row;
 }
 return hover;
}
/* 指针状态 -> 光标与按下沿。返回 0=仅光标增量更新；1=重绘场景；2=重绘并输出状态行。
 * 纯移动不再整屏重绘，也不再计算全屏校验和，快速滑动时不会闪烁。 */
static int pointer_update(uint32_t state){
 int32_t x=(int32_t)(state&0x1fffu);
 int32_t y=(int32_t)((state>>13)&0x1fffu);
 uint8_t buttons=(uint8_t)((state>>26)&7u);
 int pressed=(buttons&1u)&&!(cursor_buttons&1u);
 uint32_t old_hover=hover_state();
 cursor_restore();
 cursor_x=x;cursor_y=y;
 int scene=0,logged=0;
 if(pressed){pointer_press(x,y);scene=1;logged=1;}
 cursor_buttons=buttons;
 if(hover_state()!=old_hover)scene=1;
 if(!scene)cursor_paint();
 Line l;line_reset(&l);line_add(&l,"WM: pointer x=");line_num(&l,(uint32_t)x);
 line_add(&l," y=");line_num(&l,(uint32_t)y);line_add(&l," b=");line_num(&l,buttons);
 line_add(&l,"\n");sf_log(l.b);
 return scene?(logged?2:1):0;
}
static int text_same(const char*a,const char*b){
 if(!a||!b)return 0;
 for(uint32_t i=0;;i++){
  int x=a[i],y=b[i];
  if(x>='A'&&x<='Z')x+=32;
  if(y>='A'&&y<='Z')y+=32;
  if(x!=y)return 0;
  if(!x)return 1;
 }
}
/* 目录内按标题或 id 精确匹配（大小写不敏感）；未命中返回 -1。 */
static int app_index(const char*name){
 if(!name||!name[0])return -1;
 for(uint32_t i=0;i<launcher.count;i++)
  if(text_same(launcher.entries[i].title,name)||text_same(launcher.entries[i].id,name))return (int)i;
 return -1;
}
/* 提交一行 SFK 命令：先把原始输入写入历史，再执行动作。
 * 所有调用都发生在输入转发线程内，直接修改布局并返回供重绘。 */
static void run_shell(void){
 SfkCommand cmd;
 if(sfk_submit(&shell,&cmd)!=SFK_OK||cmd.action==SFK_NONE)return;
 Line echo;line_reset(&echo);line_add(&echo,"> ");line_add(&echo,cmd.line);
 sfk_print(&shell,echo.b);
 switch(cmd.action){
 case SFK_HELP:
  sfk_print(&shell,"HELP: COMMANDS");
  sfk_print(&shell,"APPS  OPEN <APP>  CLOSE  NEW");
  sfk_print(&shell,"WS <1-4>  LAYOUT <333|500|667|1000>");
  sfk_print(&shell,"SEARCH  CLEAR  ECHO  ABOUT  EXIT");
  break;
 case SFK_APPS:{
  Line l;line_reset(&l);line_add(&l,"APPS ");line_num(&l,launcher.count);
  sfk_print(&shell,l.b);
  for(uint32_t i=0;i<launcher.count;i++){
   Line e;line_reset(&e);
   line_add(&e,launcher.entries[i].title);line_add(&e,"  ");
   if(launcher.entries[i].window){line_add(&e,"W");line_num(&e,(uint32_t)launcher.entries[i].window);}
   else line_add(&e,"-");
   sfk_print(&shell,e.b);
  }
  break;}
 case SFK_OPEN:{
  if(!cmd.text[0]){sfk_print(&shell,"USAGE: OPEN <APP>");break;}
  int idx=app_index(cmd.text);
  if(idx<0){Line l;line_reset(&l);line_add(&l,"OPEN: NO APP ");line_add(&l,cmd.text);sfk_print(&shell,l.b);break;}
  LauncherEntry*e=&launcher.entries[idx];
  if(e->window&&tile_focus_id(&layout,e->window)==TILE_OK){
   Line l;line_reset(&l);line_add(&l,"FOCUS ");line_add(&l,e->title);sfk_print(&shell,l.b);
   break;
  }
  uint64_t id=0;
  if(tile_add(&layout,&id)!=TILE_OK){sfk_print(&shell,"OPEN: NO SPACE");break;}
  e->window=id;
  Line l;line_reset(&l);line_add(&l,"OPEN ");line_add(&l,e->title);
  line_add(&l," W");line_num(&l,(uint32_t)id);sfk_print(&shell,l.b);
  break;}
 case SFK_CLOSE:
  sfk_print(&shell,tile_close_focused(&layout)==TILE_OK?"CLOSED":"CLOSE: NO WINDOW");
  break;
 case SFK_WS:{
  if(cmd.arg<1u||cmd.arg>TILE_WORKSPACES){sfk_print(&shell,"USAGE: WS 1..4");break;}
  tile_focus_workspace(&layout,(int)cmd.arg-1-(int)layout.active);
  Line l;line_reset(&l);line_add(&l,"WS ");line_num(&l,layout.active+1);sfk_print(&shell,l.b);
  break;}
 case SFK_LAYOUT:{
  TileWorkspace*w=&layout.workspaces[layout.active];
  if(!w->count){sfk_print(&shell,"LAYOUT: NO WINDOW");break;}
  uint32_t target=cmd.arg;
  uint32_t current=w->columns[w->active].width_permille;
  if(target!=333u&&target!=500u&&target!=667u&&target!=1000u)width_preset(&layout);
  else if(target!=current)tile_set_width(&layout,(int)target-(int)current);
  Line l;line_reset(&l);line_add(&l,"COL WIDTH ");
  line_num(&l,w->columns[w->active].width_permille);sfk_print(&shell,l.b);
  break;}
 case SFK_SEARCH:
  shell_open=0;search_open=1;
  break;
 case SFK_CLEAR:
  sfk_clear(&shell);sfk_print(&shell,"SFK: HISTORY CLEARED");
  break;
 case SFK_ECHO:
  sfk_print(&shell,cmd.text[0]?cmd.text:"ECHO");
  break;
 case SFK_ABOUT:
  sfk_print(&shell,"SFK 0.1 SPIRITFOX GRAPHICAL SHELL");
  sfk_print(&shell,"BOUNDED C SHELL  NO HEAP  NO LIBC");
  break;
 case SFK_EXIT:
  shell_open=0;
  break;
 case SFK_NEW:{
  uint64_t id=0;
  if(tile_add(&layout,&id)==TILE_OK){
   Line l;line_reset(&l);line_add(&l,"NEW W");line_num(&l,(uint32_t)id);sfk_print(&shell,l.b);
  }else sfk_print(&shell,"NEW: NO SPACE");
  break;}
 case SFK_UNKNOWN:{
  Line l;line_reset(&l);line_add(&l,"UNKNOWN: ");line_add(&l,cmd.text);sfk_print(&shell,l.b);
  break;}
 default:break;
 }
}
/* 键盘不再驱动布局或叠加层：图形化的所有操作都由指针完成。
 * 只有 SFK 打开时保留文本输入（字符/退格/回车属于文本编辑而非快捷键）。 */
static int input(const uint8_t*data,uint32_t bytes){
 int changed=0;
 if(!shell_open)return 0;
 for(uint32_t i=0;i<bytes;i++){
  uint8_t key=data[i];
  if(key==0x08||key==0x7f){sfk_backspace(&shell);changed=1;}
  else if(key==0x0d||key==0x0a){run_shell();changed=1;}
  else if(key>=0x20&&key<=0x7e){sfk_push(&shell,(char)key);changed=1;}
 }
 return changed;
}
/* 注册消息 data 为 id\0title\0：只进目录，不立即建窗。 */
static void register_app(const SfMessage*m){
 uint32_t n=m->length;
 if(n<2)return;
 uint32_t i=0;while(i<n&&m->data[i])i++;
 if(i>=n)return;
 uint32_t j=0;while(i+1u+j<n&&m->data[i+1u+j])j++;
 if(i+1u+j>=n)return;
 const char*id=(const char*)m->data;
 const char*title=(const char*)(m->data+i+1u);
 if(launcher_add(&launcher,id,title)==LAUNCHER_OK){
  sf_log("WM: app ");sf_log(title);sf_log("\n");
 }
}
long sf_main(const SfBootArgs*a,size_t size){
 if(!a||size!=sizeof(*a)||a->abi_major!=1)return SF_ABI_MISMATCH;
 /* PID1 授予 framebuffer 后模块才可映射；对启动时序做有界重试。 */
 long mapped=SF_NOT_FOUND;
 for(unsigned attempt=0;attempt<200;attempt++){
  mapped=sf_sys(SYS_FB_MAP,(uint64_t)&info,0,0);
  if(mapped!=SF_DENIED)break;
  sf_yield();
 }
 if(mapped||info.size!=sizeof(info)){
  sf_log("WM: no framebuffer\n");return 1;
 }
 if(info.bpp!=32||!info.width||!info.height||info.pitch<(info.width*4u)||(info.pitch&3u)){
  sf_log("WM: unsupported bpp=");sf_number(info.bpp);sf_log("\n");return 1;
 }
 fb=(volatile uint32_t*)SF_FB;stride=info.pitch/4u;
 tile_init(&layout,info.width,info.height,WM_BAR_HEIGHT,WM_OUTER_GAP,WM_GAP);
 uint64_t id=0;if(tile_add(&layout,&id)!=TILE_OK)return 1;
 launcher_init(&launcher);
 launcher_add(&launcher,"org.spiritfox.term","TERMINAL");
 launcher_set_window(&launcher,"org.spiritfox.term",id);
 cursor_x=(int32_t)(info.width/2u);cursor_y=(int32_t)(info.height/2u);
 cursor_buttons=0;
 sfk_init(&shell);
 sfk_print(&shell,"SFK READY. TYPE HELP.");
 draw();mark_drawn();
 for(;;){
  SfMessage m;long sender=sf_recv(a->service,&m,0);
  if(sender<0)return(int)sender;
  if(m.length>SF_MSG_BYTES)m.length=SF_MSG_BYTES;
  if(m.h.opcode==OP_WM_REGISTER){register_app(&m);continue;}
  if(sender!=1)continue;
  if(m.h.opcode==OP_WM_STOP){draw();mark_drawn();sf_log("WM: stopped\n");return 0;}
  if(m.h.opcode==OP_WM_STATUS){log_state();continue;}
  if(m.h.opcode==OP_WM_POINTER){
   int mode=pointer_update((uint32_t)m.handle);
   if(mode){draw();if(mode==2)mark_drawn();}
   continue;
  }
  if(m.h.opcode!=OP_WM_INPUT||!m.length)continue;
  int changed=input(m.data,m.length);
  if(changed||redraw_needed){draw();mark_drawn();}
 }
}
