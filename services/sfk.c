#include "sfk.h"
/* 固定数组实现：输入编辑、历史上限与命令解析。所有比较只针对 ASCII，
 * 命令词大小写不敏感；非法输入不改变既有状态。 */
static int lower(int c){return c>='A'&&c<='Z'?c+32:c;}
static uint32_t text_len(const char*s){uint32_t n=0;if(!s)return 0;while(s[n])n++;return n;}
static void copy_field(char*dst,uint32_t capacity,const char*src){
 uint32_t i=0;
 if(!capacity)return;
 for(;src[i]&&i+1u<capacity;i++)dst[i]=src[i];
 dst[i]=0;
}
static int token_equal(const char*token,const char*word){
 for(uint32_t i=0;;i++){
  if(lower(token[i])!=word[i])return 0;
  if(!token[i])return 1;
 }
}
static uint32_t parse_number(const char*s,uint32_t n){
 if(!n)return 0;
 uint32_t v=0;
 for(uint32_t i=0;i<n;i++){
  if(s[i]<'0'||s[i]>'9')return 0;
  v=v*10u+(uint32_t)(s[i]-'0');
  if(v>9999u)return 0;
 }
 return v;
}
void sfk_init(Sfk*s){
 if(!s)return;
 uint8_t*bytes=(uint8_t*)s;
 for(uint32_t i=0;i<sizeof(*s);i++)bytes[i]=0;
}
int sfk_push(Sfk*s,char c){
 if(!s)return SFK_INVALID;
 if(c<0x20||c>0x7e)return SFK_INVALID;
 if(s->input_len+1u>=SFK_INPUT_BYTES)return SFK_FULL;
 s->input[s->input_len++]=c;
 s->input[s->input_len]=0;
 return SFK_OK;
}
int sfk_backspace(Sfk*s){
 if(!s)return SFK_INVALID;
 if(!s->input_len)return SFK_EMPTY;
 s->input[--s->input_len]=0;
 return SFK_OK;
}
void sfk_clear_input(Sfk*s){
 if(!s)return;
 s->input_len=0;
 s->input[0]=0;
}
int sfk_print(Sfk*s,const char*line){
 if(!s||!line)return SFK_INVALID;
 if(text_len(line)>=SFK_HISTORY_BYTES)return SFK_INVALID;
 uint32_t slot;
 if(s->history_len<SFK_HISTORY_LINES){
  slot=(s->history_start+s->history_len)%SFK_HISTORY_LINES;
  s->history_len++;
 }else{
  slot=s->history_start;
  s->history_start=(s->history_start+1u)%SFK_HISTORY_LINES;
 }
 copy_field(s->history[slot],SFK_HISTORY_BYTES,line);
 return SFK_OK;
}
void sfk_clear(Sfk*s){
 if(!s)return;
 s->history_start=0;
 s->history_len=0;
}
int sfk_submit(Sfk*s,SfkCommand*out){
 if(!s||!out)return SFK_INVALID;
 copy_field(s->last_line,SFK_INPUT_BYTES,s->input);
 s->last_arg[0]=0;
 out->action=SFK_NONE;
 out->arg=0;
 out->line=s->last_line;
 out->text=s->last_arg;
 const char*p=s->last_line;
 while(*p==' '||*p=='\t')p++;
 if(!*p){sfk_clear_input(s);return SFK_OK;}
 char token[12];
 uint32_t tl=0;
 int overlong=0;
 while(*p&&*p!=' '&&*p!='\t'){
  if(tl+1u<sizeof(token))token[tl++]=(char)lower((unsigned char)*p);
  else overlong=1;
  p++;
 }
 token[tl]=0;
 while(*p==' '||*p=='\t')p++;
 uint32_t al=0;
 while(*p&&al+1u<SFK_INPUT_BYTES)s->last_arg[al++]=*p++;
 s->last_arg[al]=0;
 while(al&&(s->last_arg[al-1]==' '||s->last_arg[al-1]=='\t'))s->last_arg[--al]=0;
 if(overlong)out->action=SFK_UNKNOWN;
 else if(token_equal(token,"help"))out->action=SFK_HELP;
 else if(token_equal(token,"apps"))out->action=SFK_APPS;
 else if(token_equal(token,"open"))out->action=SFK_OPEN;
 else if(token_equal(token,"close"))out->action=SFK_CLOSE;
 else if(token_equal(token,"ws"))out->action=SFK_WS;
 else if(token_equal(token,"layout"))out->action=SFK_LAYOUT;
 else if(token_equal(token,"search"))out->action=SFK_SEARCH;
 else if(token_equal(token,"clear"))out->action=SFK_CLEAR;
 else if(token_equal(token,"echo"))out->action=SFK_ECHO;
 else if(token_equal(token,"about"))out->action=SFK_ABOUT;
 else if(token_equal(token,"exit"))out->action=SFK_EXIT;
 else if(token_equal(token,"new"))out->action=SFK_NEW;
 else out->action=SFK_UNKNOWN;
 if(out->action==SFK_UNKNOWN)copy_field(s->last_arg,SFK_INPUT_BYTES,token);
 if(out->action==SFK_WS||out->action==SFK_LAYOUT){
  out->arg=parse_number(s->last_arg,al);
  if(out->action==SFK_WS&&out->arg>4u)out->arg=0;
 }
 sfk_clear_input(s);
 return SFK_OK;
}
uint32_t sfk_line_count(const Sfk*s){
 if(!s)return 0;
 return s->history_len;
}
const char* sfk_line_at(const Sfk*s,uint32_t index){
 if(!s||index>=s->history_len)return "";
 return s->history[(s->history_start+index)%SFK_HISTORY_LINES];
}
