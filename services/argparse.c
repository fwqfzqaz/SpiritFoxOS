#include "arguments.h"
static void put32(uint8_t*p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(i*8);}
int sf_arguments_pack(SfArguments*out,const char*const*values,unsigned count){
 if(!out||(!values&&count))return SF_INVALID;if(count>SF_ARGUMENT_MAX)return SF_NO_SPACE;
 unsigned lengths[SF_ARGUMENT_MAX],used=count*4;
 for(unsigned i=0;i<count;i++){if(!values[i])return SF_INVALID;unsigned n=0,room=SF_ARGUMENT_BYTES-used;
  while(n<room&&values[i][n])n++;if(n==room)return SF_NO_SPACE;lengths[i]=n+1;used+=n+1;
 }
 memset(out,0,sizeof(*out));out->count=count;out->bytes=used;unsigned cursor=count*4;
 for(unsigned i=0;i<count;i++){put32(out->data+i*4,sizeof(SfBootArgs)+cursor);memcpy(out->data+cursor,values[i],lengths[i]);cursor+=lengths[i];}
 return 0;
}
static int space(char c){return c==' '||c=='\t';}
int sf_arguments_split(char*line,char**values,unsigned capacity){
 if(!line||!values||!capacity||capacity>SF_ARGUMENT_MAX+1)return SF_INVALID;
 char*read=line,*write=line;unsigned count=0;
 while(*read){while(space(*read))read++;if(!*read)break;if(count==capacity)return SF_NO_SPACE;
  values[count++]=write;char quote=0;
  while(*read){char c=*read++;if(!quote&&space(c))break;
   if(c=='\\'&&quote!='\''){if(!*read)return SF_INVALID;*write++=*read++;continue;}
   if(c=='\''||c=='"'){if(!quote){quote=c;continue;}if(quote==c){quote=0;continue;}}
   *write++=c;
  }
  if(quote)return SF_INVALID;*write++=0;
 }
 return count;
}
