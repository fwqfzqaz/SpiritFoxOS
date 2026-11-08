#include "accounts.h"
#include "package.h"
static int hex_value(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;}
static int hex_decode(const char *text,uint8_t *out,size_t bytes){
 for(size_t i=0;i<bytes;i++){int high=hex_value(text[i*2]),low=hex_value(text[i*2+1]);if(high<0||low<0)return 0;out[i]=(uint8_t)((high<<4)|low);}
 return 1;
}
static int name_ok(const char *name){
 unsigned i=0;if(name[0]<'a'||name[0]>'z')return 0;
 for(;name[i];i++){char c=name[i];if(i>=SF_ACCOUNT_NAME_MAX)return 0;if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return 0;}
 return i>0;
}
static int decimal_16(const char *text,uint16_t *out){
 if(!text||!*text)return 0;uint32_t value=0;unsigned digits=0;
 for(const char *s=text;*s;s++){if(*s<'0'||*s>'9'||digits++>5)return 0;value=value*10+(unsigned)(*s-'0');if(value>65535)return 0;}
 *out=(uint16_t)value;return 1;
}
static int line_parse(char *line,SfAccountStore *out){
 if(!line[0]||line[0]=='#')return 0;
 char *fields[5];unsigned count=0;char *s=line;
 for(;;){if(count>=5)return SF_BAD_FORMAT;fields[count++]=s;while(*s&&*s!=':')s++;if(!*s){if(count!=5)return SF_BAD_FORMAT;break;}*s++=0;}
 if(out->count>=SF_ACCOUNT_MAX)return SF_NO_SPACE;
 SfAccount a;memset(&a,0,sizeof(a));
 if(!name_ok(fields[0]))return SF_BAD_FORMAT;memcpy(a.name,fields[0],strlen(fields[0])+1);
 if(!decimal_16(fields[1],&a.uid)||!decimal_16(fields[2],&a.gid))return SF_BAD_FORMAT;
 if(strlen(fields[3])!=SF_ACCOUNT_SALT_BYTES*2||strlen(fields[4])!=SF_ACCOUNT_HASH_BYTES*2)return SF_BAD_FORMAT;
 if(!hex_decode(fields[3],a.salt,sizeof(a.salt))||!hex_decode(fields[4],a.hash,sizeof(a.hash)))return SF_BAD_FORMAT;
 for(unsigned i=0;i<out->count;i++)if(!strcmp(out->accounts[i].name,a.name)||out->accounts[i].uid==a.uid)return SF_BAD_FORMAT;
 out->accounts[out->count++]=a;return 0;
}
int sf_accounts_parse(const char *text,size_t bytes,SfAccountStore *out){
 if(!text||!out||bytes>SF_ACCOUNT_TEXT_MAX)return SF_INVALID;memset(out,0,sizeof(*out));
 char line[128];size_t used=0;
 for(size_t i=0;i<=bytes;i++){
  int end=i==bytes;char c=end?0:text[i];
  if(c=='\r')return SF_BAD_FORMAT;
  if(c=='\n'||end){
   line[used]=0;int r=line_parse(line,out);used=0;if(r)return r;if(end)break;
  }else{if(used+1>=sizeof(line))return SF_BAD_FORMAT;line[used++]=c;}
 }
 unsigned roots=0;
 for(unsigned i=0;i<out->count;i++)if(!out->accounts[i].uid){roots++;if(strcmp(out->accounts[i].name,"root")||out->accounts[i].gid)return SF_BAD_FORMAT;}
 return roots==1&&out->count?0:SF_BAD_FORMAT;
}
#ifndef SF_HOST_TEST
#include "accounts_table.h"
int sf_accounts_load_builtin(SfAccountStore *out){return sf_accounts_parse(SF_ACCOUNT_BUILTIN_TEXT,SF_ACCOUNT_BUILTIN_BYTES,out);}
#else
int sf_accounts_load_builtin(SfAccountStore *out){if(!out)return SF_INVALID;memset(out,0,sizeof(*out));return SF_NOT_FOUND;}
#endif
int sf_account_lookup(const SfAccountStore *store,const char *name,const SfAccount **out){
 if(!store||!name||!out)return SF_INVALID;for(unsigned i=0;i<store->count;i++)if(!strcmp(store->accounts[i].name,name)){*out=&store->accounts[i];return 0;}return SF_NOT_FOUND;
}
int sf_account_by_uid(const SfAccountStore *store,uint32_t uid,const SfAccount **out){
 if(!store||!out)return SF_INVALID;for(unsigned i=0;i<store->count;i++)if(store->accounts[i].uid==uid){*out=&store->accounts[i];return 0;}return SF_NOT_FOUND;
}
int sf_account_verify(const SfAccount *account,const char *password){
 if(!account||!password)return SF_INVALID;size_t bytes=strlen(password);if(bytes>SF_ACCOUNT_PASSWORD_MAX)return SF_DENIED;
 uint8_t block[SF_ACCOUNT_SALT_BYTES+SF_ACCOUNT_HASH_BYTES],digest[SF_ACCOUNT_HASH_BYTES];
 memcpy(block,account->salt,SF_ACCOUNT_SALT_BYTES);memcpy(block+SF_ACCOUNT_SALT_BYTES,password,bytes);
 sf_sha256(digest,block,SF_ACCOUNT_SALT_BYTES+bytes);
 for(unsigned round=1;round<SF_ACCOUNT_HASH_ROUNDS;round++){memcpy(block,digest,SF_ACCOUNT_HASH_BYTES);memcpy(block+SF_ACCOUNT_HASH_BYTES,account->salt,SF_ACCOUNT_SALT_BYTES);sf_sha256(digest,block,sizeof(block));}
 uint8_t diff=0;for(unsigned i=0;i<SF_ACCOUNT_HASH_BYTES;i++)diff|=digest[i]^account->hash[i];
 return diff?SF_DENIED:0;
}
int sf_account_home(const SfAccount *account,char *out,size_t size){
 if(!account||!out)return SF_INVALID;if(size<sizeof("/home/")+strlen(account->name))return SF_INVALID;
 size_t prefix=sizeof("/home/")-1;memcpy(out,"/home/",prefix);memcpy(out+prefix,account->name,strlen(account->name)+1);
 if(prefix+strlen(account->name)>SF_ACCOUNT_HOME_MAX)return SF_INVALID;return 0;
}
