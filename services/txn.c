/* Bounded copy-on-write snapshots. No device/filesystem dependencies. */
#include "txn.h"
#include "package.h"
static uint32_t u32(const uint8_t*p){uint32_t n;memcpy(&n,p,4);return n;}
static uint64_t u64(const uint8_t*p){uint64_t n;memcpy(&n,p,8);return n;}
static void put32(uint8_t*p,uint32_t n){memcpy(p,&n,4);}
static void put64(uint8_t*p,uint64_t n){memcpy(p,&n,8);}
static int zero(const uint8_t*p,size_t n){while(n--)if(*p++)return 0;return 1;}
static int path_ok(const char*p){
 size_t n=0;while(n<64&&p[n])n++;if(n<2||n==64||p[0]!='/'||p[n-1]=='/')return 0;
 for(size_t i=1;i<n;i++){unsigned c=(uint8_t)p[i];if(c<33||c>126||c=='\\')return 0;if(c=='/'&&p[i-1]=='/')return 0;}
 for(size_t i=1;i<n;){size_t j=i;while(j<n&&p[j]!='/')j++;if((j-i==1&&p[i]=='.')||(j-i==2&&p[i]=='.'&&p[i+1]=='.'))return 0;i=j+1;}return 1;
}
static int sealed(const uint8_t*p){uint8_t h[32];sf_sha256(h,p,480);return !memcmp(h,p+480,32);}
static void seal(uint8_t*p){sf_sha256(p+480,p,480);}
static int decode(SfTxn*t,uint64_t generation){
 uint8_t*b=t->buffer;if(memcmp(b,"SFBANK1",8)||u64(b+8)!=generation||u32(b+16)>SF_TXN_FILES||!zero(b+20,492))return SF_BAD_FORMAT;
 unsigned count=u32(b+16);memset(t->next,0,sizeof(t->next));
 for(unsigned i=0;i<SF_TXN_FILES;i++){
  uint8_t*e=b+512+i*128,*d=b+SF_TXN_DATA+i*SF_TXN_FILE_BYTES;
  if(i>=count){if(!zero(e,128)||!zero(d,SF_TXN_FILE_BYTES))return SF_BAD_FORMAT;continue;}
  uint32_t n=u32(e+64);if(!path_ok((char*)e)||n>SF_TXN_FILE_BYTES||!zero(e+strlen((char*)e)+1,63-strlen((char*)e))||!zero(e+68,28)||!zero(d+n,SF_TXN_FILE_BYTES-n))return SF_BAD_FORMAT;
  uint8_t h[32];sf_sha256(h,d,n);if(memcmp(h,e+96,32))return SF_BAD_FORMAT;
  for(unsigned j=0;j<i;j++)if(!strcmp(t->next[j].path,(char*)e))return SF_BAD_FORMAT;
  memcpy(t->next[i].path,e,64);t->next[i].bytes=n;memcpy(t->next[i].data,d,n);
 }
 return zero(b+SF_TXN_DATA+SF_TXN_FILES*SF_TXN_FILE_BYTES,sizeof(t->buffer)-SF_TXN_DATA-SF_TXN_FILES*SF_TXN_FILE_BYTES)?0:SF_BAD_FORMAT;
}
int sf_txn_mount(SfTxn*t,SfTxnRead read,SfTxnWrite write,SfTxnFlush flush,void*context){
 t->ready=0;t->generation=0;t->read=read;t->write=write;t->flush=flush;t->context=context;
 uint8_t s[512],hash[32];int r=read(context,SF_TXN_HEADER,s);if(r)return r;
 if(memcmp(s,"SFTXN1\0",8)||u32(s+8)!=1||u32(s+12)!=SF_TXN_SECTORS||!zero(s+32,448)||!sealed(s))return SF_BAD_FORMAT;
 memcpy(t->volume,s+16,16);
 for(unsigned slot=0;slot<2;slot++){
  r=read(context,SF_TXN_HEADER+1+slot,s);if(r)return r;
  if(memcmp(s,"SFCMT1\0",8)||!sealed(s)||!u64(s+8)||u32(s+16)!=slot||u32(s+20)||memcmp(s+24,t->volume,16)||!zero(s+72,408))continue;
  uint64_t generation=u64(s+8);memcpy(hash,s+40,32);
  for(unsigned i=0;i<SF_TXN_BANK_SECTORS;i++){r=read(context,SF_TXN_BANK+slot*SF_TXN_BANK_SECTORS+i,t->buffer+i*512);if(r)return r;}
  uint8_t h[32];sf_sha256(h,t->buffer,sizeof(t->buffer));if(memcmp(h,hash,32)||decode(t,generation))continue;
  if(generation==t->generation)return SF_BAD_FORMAT;
  if(generation>t->generation){t->generation=generation;t->bank=slot;memcpy(t->files,t->next,sizeof(t->files));}
 }
 if(!t->generation)return SF_BAD_FORMAT;t->ready=1;return 0;
}
long sf_txn_get(SfTxn*t,const char*path,void*out,size_t capacity){
 if(!t->ready)return SF_DEVICE_GONE;
 for(unsigned i=0;i<SF_TXN_FILES;i++)if(!strcmp(t->files[i].path,path)){if(t->files[i].bytes>capacity)return SF_NO_SPACE;memcpy(out,t->files[i].data,t->files[i].bytes);return t->files[i].bytes;}
 return SF_NOT_FOUND;
}
int sf_txn_resize(SfTxn*t,const char*path,uint64_t bytes){
 if(!t->ready)return SF_DEVICE_GONE;if(!path_ok(path))return SF_INVALID;if(bytes>SF_TXN_FILE_BYTES)return SF_NO_SPACE;
 unsigned i=0;while(i<SF_TXN_FILES&&strcmp(t->files[i].path,path))i++;uint32_t old=i<SF_TXN_FILES?t->files[i].bytes:0;
 /* An absent file is the adapter's empty file; a same-length resize is a
  * read-only no-op, including an empty file not yet represented on disk. */
 if(bytes==old)return 0;
 memset(t->buffer,0,bytes);if(i<SF_TXN_FILES)memcpy(t->buffer,t->files[i].data,old<bytes?old:bytes);
 const char*paths[]={path};const void*data[]={t->buffer};uint32_t lengths[]={bytes};
 /* commit consumes this scratch input into next before rebuilding buffer. */
 return sf_txn_commit(t,paths,data,lengths,1);
}
int sf_txn_commit(SfTxn*t,const char*const*paths,const void*const*data,const uint32_t*bytes,unsigned count){
 if(!t->ready)return SF_DEVICE_GONE;if(!count||count>SF_TXN_FILES||t->generation==UINT64_MAX)return SF_INVALID;
 memcpy(t->next,t->files,sizeof(t->next));
 for(unsigned n=0;n<count;n++){
  if(!path_ok(paths[n])||bytes[n]>SF_TXN_FILE_BYTES)return SF_INVALID;
  for(unsigned j=0;j<n;j++)if(!strcmp(paths[j],paths[n]))return SF_INVALID;
  unsigned i=0;while(i<SF_TXN_FILES&&t->next[i].path[0]&&strcmp(t->next[i].path,paths[n]))i++;if(i==SF_TXN_FILES)return SF_NO_SPACE;
  memset(&t->next[i],0,sizeof(t->next[i]));memcpy(t->next[i].path,paths[n],strlen(paths[n])+1);t->next[i].bytes=bytes[n];memcpy(t->next[i].data,data[n],bytes[n]);
 }
 uint64_t generation=t->generation+1;unsigned bank=t->bank^1;uint8_t s[512]={0},h[32];
 memset(t->buffer,0,sizeof(t->buffer));memcpy(t->buffer,"SFBANK1",8);put64(t->buffer+8,generation);
 unsigned files=0;while(files<SF_TXN_FILES&&t->next[files].path[0])files++;put32(t->buffer+16,files);
 for(unsigned i=0;i<files;i++){uint8_t*e=t->buffer+512+i*128,*d=t->buffer+SF_TXN_DATA+i*SF_TXN_FILE_BYTES;memcpy(e,t->next[i].path,64);put32(e+64,t->next[i].bytes);memcpy(d,t->next[i].data,t->next[i].bytes);sf_sha256(e+96,d,t->next[i].bytes);}
 sf_sha256(h,t->buffer,sizeof(t->buffer));
 /* Invalidate old target record durably before reusing its data bank. */
 if(t->write(t->context,SF_TXN_HEADER+1+bank,s)||t->flush(t->context))goto uncertain;
 for(unsigned i=0;i<SF_TXN_BANK_SECTORS;i++)if(t->write(t->context,SF_TXN_BANK+bank*SF_TXN_BANK_SECTORS+i,t->buffer+i*512))goto uncertain;
 if(t->flush(t->context))goto uncertain;
 memcpy(s,"SFCMT1\0",8);put64(s+8,generation);put32(s+16,bank);memcpy(s+24,t->volume,16);memcpy(s+40,h,32);seal(s);
 if(t->write(t->context,SF_TXN_HEADER+1+bank,s)||t->flush(t->context))goto uncertain;
 memcpy(t->files,t->next,sizeof(t->files));t->generation=generation;t->bank=bank;return 0;
uncertain:
 t->ready=0;return SF_COMMIT_UNCERTAIN;
}
