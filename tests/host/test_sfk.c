#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sfk.h"
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"SFK FAIL line %d\n",__LINE__);exit(1);}}while(0)
static void type(Sfk*s,const char*text){for(;*text;text++)CHECK(sfk_push(s,*text)==SFK_OK);}
static SfkCommand submit(Sfk*s){SfkCommand c;CHECK(sfk_submit(s,&c)==SFK_OK);return c;}
int main(void){
 Sfk s;
 sfk_init(&s);
 CHECK(!s.input_len&&!s.input[0]&&!s.history_len&&!s.history_start);
 CHECK(sfk_line_count(&s)==0);
 CHECK(!strcmp(sfk_line_at(&s,0),"")&&!strcmp(sfk_line_at(&s,1),""));

 /* 输入编辑：只接受可打印 ASCII，容量 47 字符，满后无副作用。 */
 type(&s,"help");
 CHECK(s.input_len==4&&!strcmp(s.input,"help"));
 CHECK(sfk_backspace(&s)==SFK_OK&&s.input_len==3&&!strcmp(s.input,"hel"));
 sfk_clear_input(&s);
 CHECK(!s.input_len&&!s.input[0]);
 CHECK(sfk_backspace(&s)==SFK_EMPTY);
 CHECK(sfk_push(&s,0x01)==SFK_INVALID&&sfk_push(&s,(char)0x7f)==SFK_INVALID);
 CHECK(!s.input_len);
 for(unsigned i=0;i<SFK_INPUT_BYTES-1u;i++)CHECK(sfk_push(&s,(char)('a'+i%26))==SFK_OK);
 CHECK(s.input_len==SFK_INPUT_BYTES-1u&&s.input[s.input_len]==0);
 CHECK(sfk_push(&s,'z')==SFK_FULL);
 CHECK(s.input_len==SFK_INPUT_BYTES-1u);
 sfk_clear_input(&s);
 CHECK(sfk_push(&s,0)==SFK_INVALID&&sfk_push(&s,(char)0x80)==SFK_INVALID);

 /* 历史上限 12 行：超出后淘汰最旧；非法行不改变状态。 */
 sfk_init(&s);
 CHECK(sfk_print(&s,0)==SFK_INVALID);
 {
  char too_long[SFK_HISTORY_BYTES+1];
  for(unsigned i=0;i<SFK_HISTORY_BYTES;i++)too_long[i]='a';
  too_long[SFK_HISTORY_BYTES]=0;
  CHECK(sfk_print(&s,too_long)==SFK_INVALID);
  CHECK(!sfk_line_count(&s));
 }
 for(unsigned i=0;i<14;i++){
  char line[8];line[0]='L';line[1]=(char)('0'+(i/10));line[2]=(char)('0'+(i%10));line[3]=0;
  CHECK(sfk_print(&s,line)==SFK_OK);
 }
 CHECK(sfk_line_count(&s)==SFK_HISTORY_LINES);
 CHECK(!strcmp(sfk_line_at(&s,0),"L02")&&!strcmp(sfk_line_at(&s,11),"L13"));
 CHECK(!strcmp(sfk_line_at(&s,12),""));
 sfk_clear(&s);
 CHECK(!sfk_line_count(&s)&&!strcmp(sfk_line_at(&s,0),""));

 /* 空输入 / 纯空白：无历史、不产生命令。 */
 type(&s,"   ");
 {
  SfkCommand c=submit(&s);
  CHECK(c.action==SFK_NONE&&!c.arg&&!c.text[0]&&!strcmp(c.line,"   "));
  CHECK(!s.input_len&&!sfk_line_count(&s));
 }

 /* 命令解析大小写不敏感，参数原样保留。 */
 type(&s,"  Help ");
 {
  SfkCommand c=submit(&s);
  CHECK(c.action==SFK_HELP&&!c.arg&&!c.text[0]);
  CHECK(!strcmp(c.line,"  Help ")&&!s.input_len);
 }
 type(&s,"Apps");CHECK(submit(&s).action==SFK_APPS);
 type(&s,"open NoTeS");
 {
  SfkCommand c=submit(&s);
  CHECK(c.action==SFK_OPEN&&!c.arg&&!strcmp(c.text,"NoTeS"));
 }
 type(&s,"open");
 {
  SfkCommand c=submit(&s);
  CHECK(c.action==SFK_OPEN&&!c.text[0]);
 }
 type(&s,"close");CHECK(submit(&s).action==SFK_CLOSE);
 type(&s,"ws 2");
 {
  SfkCommand c=submit(&s);
  CHECK(c.action==SFK_WS&&c.arg==2&&!strcmp(c.text,"2"));
 }
 /* 非法工作区编号：解析为 0，由调用方提示用法。 */
 type(&s,"ws 9");CHECK(submit(&s).arg==0);
 type(&s,"ws 0");CHECK(submit(&s).arg==0);
 type(&s,"ws two");CHECK(submit(&s).arg==0);
 type(&s,"ws");CHECK(submit(&s).arg==0);
 type(&s,"layout 667");
 {
  SfkCommand c=submit(&s);
  CHECK(c.action==SFK_LAYOUT&&c.arg==667&&!strcmp(c.text,"667"));
 }
 type(&s,"layout abc");CHECK(submit(&s).arg==0);
 type(&s,"search");CHECK(submit(&s).action==SFK_SEARCH);
 type(&s,"clear");CHECK(submit(&s).action==SFK_CLEAR);
 type(&s,"echo hello world");
 {
  SfkCommand c=submit(&s);
  CHECK(c.action==SFK_ECHO&&!strcmp(c.text,"hello world"));
 }
 type(&s,"echo");
 {
  SfkCommand c=submit(&s);
  CHECK(c.action==SFK_ECHO&&!c.text[0]);
 }
 type(&s,"about");CHECK(submit(&s).action==SFK_ABOUT);
 type(&s,"exit");CHECK(submit(&s).action==SFK_EXIT);
 type(&s,"new");CHECK(submit(&s).action==SFK_NEW);
 type(&s,"quit");
 {
  SfkCommand c=submit(&s);
  CHECK(c.action==SFK_UNKNOWN&&!strcmp(c.text,"quit"));
 }
 type(&s,"UNKNOWNCOMMAND");
 {
  SfkCommand c=submit(&s);
  CHECK(c.action==SFK_UNKNOWN&&!strcmp(c.text,"unknowncomm"));
 }

 /* 超长命令词：按未知命令处理，不越界。 */
 sfk_init(&s);
 for(unsigned i=0;i<SFK_INPUT_BYTES-1u;i++)CHECK(sfk_push(&s,(char)('a'+i%26))==SFK_OK);
 {
  SfkCommand c=submit(&s);
  CHECK(c.action==SFK_UNKNOWN&&!strcmp(c.text,"abcdefghijk"));
 }
 /* 输出指针为空：拒绝且保留输入。 */
 sfk_init(&s);
 type(&s,"help");
 CHECK(sfk_submit(&s,0)==SFK_INVALID&&s.input_len==4&&!strcmp(s.input,"help"));
 CHECK(sfk_submit(&s,0)==SFK_INVALID);
 /* 提交只清空输入，原始行仍可由命令引用。 */
 {
  SfkCommand c=submit(&s);
  CHECK(c.action==SFK_HELP&&!s.input_len&&!strcmp(c.line,"help"));
 }
 /* 历史记录跨提交保留，且一行一写。 */
 CHECK(sfk_print(&s,"one")==SFK_OK);
 CHECK(sfk_print(&s,"two")==SFK_OK);
 CHECK(sfk_line_count(&s)==2&&!strcmp(sfk_line_at(&s,0),"one")&&!strcmp(sfk_line_at(&s,1),"two"));
 printf("SFK PASS: %u checks, input/history/parse/actions\n",checks);
 return 0;
}
