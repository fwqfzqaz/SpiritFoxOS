#include "arguments.h"
static void put32(uint8_t*p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(i*8);}
int sf_invocation_pack(SfArguments*out,const char*const*values,unsigned count,const char*const*environment,unsigned env_count){
 if(!out||(!values&&count)||(!environment&&env_count))return SF_INVALID;if(count>SF_ARGUMENT_MAX||env_count>SF_ENVIRONMENT_MAX)return SF_NO_SPACE;
 unsigned total=count+env_count,lengths[SF_ARGUMENT_MAX+SF_ENVIRONMENT_MAX],used=total*4;
 for(unsigned i=0;i<total;i++){const char*s=i<count?values[i]:environment[i-count];if(!s)return SF_INVALID;unsigned n=0,room=SF_ARGUMENT_BYTES-used;
  while(n<room&&s[n])n++;if(n==room)return SF_NO_SPACE;lengths[i]=n+1;used+=n+1;
  if(i>=count){unsigned key=0;while(key<n&&s[key]!='=')key++;if(key==n||!sf_environment_name_valid(s,key))return SF_INVALID;
   for(unsigned j=0;j<i-count;j++){const char*other=environment[j];unsigned k=0;while(other[k]&&other[k]!='=')k++;if(k==key&&!memcmp(s,other,key))return SF_INVALID;}}
 }
 memset(out,0,sizeof(*out));out->count=count;out->env_count=env_count;out->bytes=used;unsigned cursor=total*4;
 for(unsigned i=0;i<total;i++){const char*s=i<count?values[i]:environment[i-count];put32(out->data+i*4,sizeof(SfBootArgs)+cursor);memcpy(out->data+cursor,s,lengths[i]);cursor+=lengths[i];}
 return 0;
}
int sf_arguments_pack(SfArguments*out,const char*const*values,unsigned count){return sf_invocation_pack(out,values,count,0,0);}
static int space(char c){return c==' '||c=='\t';}
int sf_arguments_split(char*line,char**values,unsigned capacity){
 if(!line||!values||!capacity||capacity>SF_ARGUMENT_MAX+SF_ENVIRONMENT_MAX+2)return SF_INVALID;
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
