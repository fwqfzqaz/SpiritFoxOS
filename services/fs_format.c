/* Disk format registry: keep SFRFS1 byte-compatible and add SFRFS2. */
#include "fs_format.h"
static int zero(const uint8_t*p,size_t n){while(n--)if(*p++)return 0;return 1;}
int sf_fs_probe(SfSectorRead read,void*context,uint64_t capacity,uint64_t*format){
 if(!read||capacity<1024||(capacity&511))return SF_INVALID;
 uint8_t header[512];int r=read(context,0,header);if(r)return r;
 if(!memcmp(header,"SFRFS1\0\0",8)){if(format)*format=SF_FS_FORMAT_SFRFS1;return 0;}
 if(!memcmp(header,"SFRFS2\0\0",8)){if(format)*format=SF_FS_FORMAT_SFRFS2;return 0;}
 return SF_BAD_FORMAT;
}
static int mount2(SfReadFs*fs,SfSectorRead read,void*context,uint64_t capacity){
 memset(fs,0,sizeof(*fs));if(!read||capacity<1024||capacity>SF_READFS2_IMAGE_MAX||(capacity&511))return SF_BAD_FORMAT;
 uint8_t header[512],table[SF_READFS2_MAX_ENTRIES*128];int r=read(context,0,header);if(r)return r;
 if(memcmp(header,"SFRFS2\0\0",8)||sf_u32(header+8)!=1||sf_u32(header+12)!=512||sf_u64(header+16)!=capacity||sf_u32(header+28)!=128||sf_u64(header+32)!=512||!zero(header+80,432))return SF_BAD_FORMAT;
 uint32_t count=sf_u32(header+24);uint64_t bytes=sf_u64(header+40);
 if(count>SF_READFS2_MAX_ENTRIES||bytes!=count*128ull||512+bytes>capacity)return SF_BAD_FORMAT;
 for(uint64_t off=0;off<bytes;off+=512){uint8_t sector[512];r=read(context,1+off/512,sector);if(r)return r;size_t n=bytes-off;if(n>512)n=512;memcpy(table+off,sector,n);if(n<512&&!zero(sector+n,512-n))return SF_BAD_FORMAT;}
 uint8_t digest[32];sf_sha256(digest,table,bytes);if(memcmp(digest,header+48,32))return SF_BAD_FORMAT;
 uint64_t data_start=(512+bytes+511)&~511ull;
 for(unsigned i=0;i<count;i++){
  const uint8_t*e=table+i*128;size_t len=0;while(len<64&&e[len])len++;
  if(len==64||!zero(e+len,64-len)||sf_u32(e+112)!=1||!zero(e+116,12))return SF_BAD_FORMAT;
  SfReadFile*f=&fs->files[i];memcpy(f->path,e,64);f->offset=sf_u64(e+64);f->bytes=sf_u64(e+72);memcpy(f->hash,e+80,32);
  if(!sf_readfs_path(f->path)||f->bytes>SF_READFS2_FILE_MAX||(f->offset&511)||f->offset<data_start||f->offset>capacity||f->bytes>capacity-f->offset)return SF_BAD_FORMAT;
  if(i&&strcmp(fs->files[i-1].path,f->path)>=0)return SF_BAD_FORMAT;
  for(unsigned j=0;j<i;j++){SfReadFile*g=&fs->files[j];if(f->bytes&&g->bytes&&f->offset<g->offset+g->bytes&&g->offset<f->offset+f->bytes)return SF_BAD_FORMAT;size_t n=strlen(g->path);if(!memcmp(g->path,f->path,n)&&f->path[n]=='/')return SF_BAD_FORMAT;}
 }
 fs->read=read;fs->context=context;fs->capacity=capacity;fs->count=count;fs->mounted=1;return 0;
}
int sf_fs_mount_auto(SfReadFs*fs,SfSectorRead read,void*context,uint64_t capacity,int hint,int*format){
 if(!fs||!read||hint<SF_FS_FORMAT_AUTO||hint>SF_FS_FORMAT_SFRFS2)return SF_INVALID;
 int detected=0;
 if(hint==SF_FS_FORMAT_SFRFS1)detected=SF_FS_FORMAT_SFRFS1;
 else if(hint==SF_FS_FORMAT_SFRFS2)detected=SF_FS_FORMAT_SFRFS2;
 else{uint64_t probe=0;int r=sf_fs_probe(read,context,capacity,&probe);if(r)return r;detected=(int)probe;}
 int r=detected==SF_FS_FORMAT_SFRFS1?sf_readfs_mount(fs,read,context,capacity):detected==SF_FS_FORMAT_SFRFS2?mount2(fs,read,context,capacity):SF_BAD_FORMAT;
 if(r)return r;if(format)*format=detected;return 0;
}
