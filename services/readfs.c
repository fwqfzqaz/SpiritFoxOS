/* SFRFS1 is a small, fixed-directory, read-only disk format; see docs/STORAGE.md. */
#include "readfs.h"
static int zero(const uint8_t*p,size_t n){while(n--)if(*p++)return 0;return 1;}
int sf_readfs_path(const char*p){
 size_t n=strlen(p);if(n<2||n>=64||p[0]!='/'||p[n-1]=='/')return 0;
 /* This first on-disk profile deliberately restricts names to printable ASCII. */
 for(size_t i=0;i<n;i++)if((uint8_t)p[i]<33||(uint8_t)p[i]>126||p[i]=='\\')return 0;
 for(const char*s=p+1;*s;){const char*b=s;while(*s&&*s!='/')s++;size_t len=s-b;if(!len||(len==1&&b[0]=='.')||(len==2&&b[0]=='.'&&b[1]=='.'))return 0;if(*s)s++;}
 return 1;
}
int sf_readfs_mount(SfReadFs*fs,SfSectorRead read,void*context,uint64_t capacity){
 memset(fs,0,sizeof(*fs));if(!read||capacity<1024||capacity>SF_READFS_IMAGE_MAX||(capacity&511))return SF_BAD_FORMAT;
 uint8_t header[512],table[SF_READFS_MAX_ENTRIES*128];int r=read(context,0,header);if(r)return r;
 if(memcmp(header,"SFRFS1\0\0",8)||sf_u32(header+8)!=1||sf_u32(header+12)!=512||sf_u64(header+16)!=capacity||sf_u32(header+28)!=128||sf_u64(header+32)!=512||!zero(header+80,432))return SF_BAD_FORMAT;
 uint32_t count=sf_u32(header+24);uint64_t bytes=sf_u64(header+40);
 if(!count||count>SF_READFS_MAX_ENTRIES||bytes!=count*128ull||512+bytes>capacity)return SF_BAD_FORMAT;
 for(uint64_t off=0;off<bytes;off+=512){uint8_t sector[512];r=read(context,1+off/512,sector);if(r)return r;size_t n=bytes-off;if(n>512)n=512;memcpy(table+off,sector,n);if(n<512&&!zero(sector+n,512-n))return SF_BAD_FORMAT;}
 uint8_t digest[32];sf_sha256(digest,table,bytes);if(memcmp(digest,header+48,32))return SF_BAD_FORMAT;
 uint64_t data_start=(512+bytes+511)&~511ull;
 for(unsigned i=0;i<count;i++){
  const uint8_t*e=table+i*128;size_t len=0;while(len<64&&e[len])len++;
  if(len==64||!zero(e+len,64-len)||sf_u32(e+112)!=1||!zero(e+116,12))return SF_BAD_FORMAT;
  SfReadFile*f=&fs->files[i];memcpy(f->path,e,64);f->offset=sf_u64(e+64);f->bytes=sf_u64(e+72);memcpy(f->hash,e+80,32);
  if(!sf_readfs_path(f->path)||!f->bytes||f->bytes>SF_READFS_FILE_MAX||(f->offset&511)||f->offset<data_start||f->offset>capacity||f->bytes>capacity-f->offset)return SF_BAD_FORMAT;
  if(i&&strcmp(fs->files[i-1].path,f->path)>=0)return SF_BAD_FORMAT;
  for(unsigned j=0;j<i;j++){SfReadFile*g=&fs->files[j];if(f->offset<g->offset+g->bytes&&g->offset<f->offset+f->bytes)return SF_BAD_FORMAT;size_t n=strlen(g->path);if(!memcmp(g->path,f->path,n)&&f->path[n]=='/')return SF_BAD_FORMAT;}
 }
 fs->read=read;fs->context=context;fs->capacity=capacity;fs->count=count;fs->mounted=1;return 0;
}
int sf_readfs_find(const SfReadFs*fs,const char*path){if(!fs->mounted)return SF_DEVICE_GONE;if(!sf_readfs_path(path))return SF_INVALID;for(unsigned i=0;i<fs->count;i++)if(!strcmp(fs->files[i].path,path))return i;return SF_NOT_FOUND;}
long sf_readfs_read(SfReadFs*fs,unsigned index,uint64_t offset,void*out,size_t bytes){
 if(!fs->mounted)return SF_DEVICE_GONE;if(index>=fs->count)return SF_INVALID;SfReadFile*f=&fs->files[index];if(offset>=f->bytes||!bytes)return 0;
 if(bytes>f->bytes-offset)bytes=f->bytes-offset;size_t done=0;
 while(done<bytes){uint64_t position=f->offset+offset+done;uint8_t sector[512];int r=fs->read(fs->context,position/512,sector);if(r)return r;size_t n=512-(position&511);if(n>bytes-done)n=bytes-done;memcpy((uint8_t*)out+done,sector+(position&511),n);done+=n;}
 return done;
}
long sf_readfs_load(SfReadFs*fs,unsigned index,void*out,size_t capacity){
 if(!fs->mounted)return SF_DEVICE_GONE;if(index>=fs->count)return SF_INVALID;SfReadFile*f=&fs->files[index];if(f->bytes>capacity)return SF_NO_SPACE;
 long r=sf_readfs_read(fs,index,0,out,f->bytes);if(r<0)return r;if((uint64_t)r!=f->bytes)return SF_BAD_FORMAT;uint8_t hash[32];sf_sha256(hash,out,f->bytes);if(memcmp(hash,f->hash,32)){memset(out,0,f->bytes);return SF_BAD_FORMAT;}return r;
}
