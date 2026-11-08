#include <stdio.h>
#include <stdlib.h>
#include "launcher.h"
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"LAUNCHER FAIL line %d\n",__LINE__);exit(1);}}while(0)
static void type(Launcher*l,const char*s){for(;*s;s++)CHECK(launcher_query_push(l,*s)==LAUNCHER_OK);}
int main(void){
 Launcher l;
 launcher_init(&l);
 CHECK(!launcher_count(&l)&&!l.selected&&!l.query_len&&!l.query[0]);
 CHECK(launcher_at(&l,0)==0);
 uint32_t out[LAUNCHER_APPS];
 CHECK(launcher_search(&l,out,LAUNCHER_APPS)==0);

 /* 新增、重复 id 更新标题并保留窗口 */
 CHECK(launcher_add(&l,"org.spiritfox.term","TERMINAL")==LAUNCHER_OK);
 CHECK(launcher_count(&l)==1&&!launcher_at(&l,0)->window);
 CHECK(launcher_set_window(&l,"org.spiritfox.term",7)==LAUNCHER_OK);
 CHECK(launcher_add(&l,"org.spiritfox.term","TERMINAL2")==LAUNCHER_OK);
 CHECK(launcher_count(&l)==1);
 CHECK(launcher_at(&l,0)->window==7);
 CHECK(launcher_set_window(&l,"org.spiritfox.term",0)==LAUNCHER_INVALID);
 CHECK(launcher_set_window(&l,"org.spiritfox.missing",9)==LAUNCHER_NOT_FOUND);

 /* 非法输入无副作用 */
 {
  uint32_t before=launcher_count(&l);
  CHECK(launcher_add(&l,"","X")==LAUNCHER_INVALID);
  CHECK(launcher_add(&l,"org.spiritfox.abcdefghijklmnopqrstuvwxyz012345","X")==LAUNCHER_INVALID);
  CHECK(launcher_add(&l,"org.spiritfox.long","ABCDEFGHIJKLMNOPQRSTUVWX")==LAUNCHER_INVALID);
  CHECK(launcher_count(&l)==before);
 }
 launcher_init(&l);
 CHECK(launcher_add(&l,"org.spiritfox.files","FILES")==LAUNCHER_OK);
 CHECK(launcher_add(&l,"org.spiritfox.notes","NOTES")==LAUNCHER_OK);
 CHECK(launcher_add(&l,"org.spiritfox.settings","SETTINGS")==LAUNCHER_OK);
 CHECK(launcher_add(&l,"org.spiritfox.term","TERMINAL")==LAUNCHER_OK);
 CHECK(launcher_count(&l)==4);
 CHECK(launcher_at(&l,0)->window==0&&launcher_at(&l,3)->window==0);
 CHECK(launcher_at(&l,4)==0);

 /* 空查询全量且顺序稳定 */
 CHECK(launcher_search(&l,out,LAUNCHER_APPS)==4);
 for(uint32_t i=0;i<4;i++)CHECK(out[i]==i);
 /* 容量不足只写前 capacity，但返回完整匹配数 */
 CHECK(launcher_search(&l,out,2)==4&&out[0]==0&&out[1]==1);

 /* 大小写不敏感子串：标题与 id 均参与 */
 type(&l,"FIL");
 CHECK(l.query_len==3&&!l.selected);
 CHECK(launcher_search(&l,out,LAUNCHER_APPS)==1&&out[0]==0);
 launcher_query_clear(&l);
 type(&l,"tEr");
 CHECK(launcher_search(&l,out,LAUNCHER_APPS)==1&&out[0]==3);
 launcher_query_clear(&l);
 type(&l,"spiritfox.notes");
 CHECK(l.query_len==15);
 CHECK(launcher_search(&l,out,LAUNCHER_APPS)==1&&out[0]==1);
 launcher_query_clear(&l);
 type(&l,"set");
 CHECK(launcher_search(&l,out,LAUNCHER_APPS)==1&&out[0]==2);
 launcher_query_clear(&l);

 /* 查询长度上限 23 个字符；退格与清空 */
 for(unsigned i=0;i<LAUNCHER_QUERY_BYTES-1u;i++)CHECK(launcher_query_push(&l,(char)('a'+i%26))==LAUNCHER_OK);
 CHECK(l.query_len==LAUNCHER_QUERY_BYTES-1u);
 CHECK(launcher_query_push(&l,'z')==LAUNCHER_NO_SPACE);
 CHECK(l.query_len==LAUNCHER_QUERY_BYTES-1u);
 CHECK(launcher_query_push(&l,1)==LAUNCHER_INVALID);
 CHECK(launcher_query_pop(&l)==LAUNCHER_OK);
 CHECK(l.query_len==LAUNCHER_QUERY_BYTES-2u);
 launcher_query_clear(&l);
 CHECK(!l.query_len&&!l.query[0]);
 CHECK(launcher_query_pop(&l)==LAUNCHER_NOT_FOUND);

 /* 选择夹紧 */
 launcher_init(&l);
 CHECK(launcher_add(&l,"org.spiritfox.files","FILES")==LAUNCHER_OK);
 CHECK(launcher_add(&l,"org.spiritfox.notes","NOTES")==LAUNCHER_OK);
 CHECK(launcher_add(&l,"org.spiritfox.settings","SETTINGS")==LAUNCHER_OK);
 CHECK(launcher_add(&l,"org.spiritfox.term","TERMINAL")==LAUNCHER_OK);
 CHECK(launcher_select(&l,1)==LAUNCHER_OK&&l.selected==1);
 CHECK(launcher_select(&l,9)==LAUNCHER_OK&&l.selected==3);
 CHECK(launcher_select(&l,1)==LAUNCHER_OK&&l.selected==3);
 CHECK(launcher_select(&l,-100)==LAUNCHER_OK&&l.selected==0);
 type(&l,"notes");
 CHECK(l.selected==0);
 CHECK(launcher_select(&l,1)==LAUNCHER_OK&&l.selected==0);
 CHECK(launcher_search(&l,out,LAUNCHER_APPS)==1&&out[0]==1);
 launcher_query_clear(&l);

 /* 满容量：第 17 个应用被拒绝且状态不变 */
 launcher_init(&l);
 char id[LAUNCHER_ID_BYTES];
 for(unsigned i=0;i<LAUNCHER_APPS;i++){
  id[0]='a';id[1]=(char)('a'+(i%26));id[2]=(char)('a'+((i/26)%26));id[3]=(char)('0'+(i%10));
  id[4]=(char)('0'+((i/10)%10));id[5]=0;
  CHECK(launcher_add(&l,id,"APP")==LAUNCHER_OK);
 }
 CHECK(launcher_count(&l)==LAUNCHER_APPS);
 {
  uint32_t before=launcher_count(&l);uint64_t window=launcher_at(&l,LAUNCHER_APPS-1)->window;
  CHECK(launcher_add(&l,"overflow","OVER")==LAUNCHER_NO_SPACE);
  CHECK(launcher_count(&l)==before&&launcher_at(&l,LAUNCHER_APPS-1)->window==window);
 }
 printf("LAUNCHER PASS: %u checks, catalog/search/query/selection/capacity\n",checks);
 return 0;
}
